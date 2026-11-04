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
#include "Engine/Nodes/Filter/GodRays.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/Resampler.h"
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

// The largest absolute difference between two renders of the same window, or -1 when they differ
// in size.
float
maxDifference(const RenderedPlane& a,
              const RenderedPlane& b)
{
    if (a.pixels.size() != b.pixels.size()) {
        ADD_FAILURE() << "the renders differ in size";

        return -1.f;
    }
    float worst = 0.f;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        worst = std::max(worst, std::fabs(a.pixels[i] - b.pixels[i]));
    }

    return worst;
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

class NativeGodRaysTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeGodRays64x48", 1.));
    }

    // An opaque Constant of value v on RGB over the project format.
    NodePtr makeConstant(double v)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        EXPECT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentProject)));
        EXPECT_TRUE(setKnobValues(node, kConstantParamColor, { v, v, v, 1. }));

        return node;
    }

    NodePtr makeParitySource(int x,
                             int y)
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, x, y);
        }

        return source;
    }

    // A GodRays of source centred on the format's centre, with the bilinear filter, which copies
    // pixels exactly under whole-pixel translations.
    NodePtr makeGodRays(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GODRAYS), PLUGIN_MAJOR_NATRON_GODRAYS);
        EXPECT_TRUE(isNative(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kGodRaysInputSource, true);
        }
        EXPECT_TRUE(setKnobValues(node, kGodRaysParamCenter, { kCenterX, kCenterY }));
        EXPECT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("bilinear")));

        return node;
    }
};

TEST_F(NativeGodRaysTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_GODRAYS, PLUGIN_MAJOR_NATRON_GODRAYS, "GodRays");
}

TEST_F(NativeGodRaysTest, InputsAndKnobs)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GODRAYS), PLUGIN_MAJOR_NATRON_GODRAYS);
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(2, node->getNInputs());
    EXPECT_EQ(std::string("Source"), node->getInputLabel(kGodRaysInputSource));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(kGodRaysInputMask));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(kGodRaysInputMask));
    EXPECT_TRUE(node->getEffectInstance()->isInputOptional(kGodRaysInputMask));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(node->getKnobByName(kOfxMixParamName)));
    EXPECT_TRUE(bool(node->getKnobByName(kOfxMaskInvertParamName)));
    // A filter: no (un)premult-by knob.
    EXPECT_FALSE(bool(node->getKnobByName("hostUnPremultBy")));

    const struct {
        const char* name;
        const char* label;
        bool secret;
    } knobs[] = {
        { kGodRaysParamTranslate, "Translate", false },
        { kGodRaysParamRotate, "Rotate", false },
        { kGodRaysParamScale, "Scale", false },
        { kGodRaysParamSkewX, "Skew X", false },
        { kGodRaysParamSkewY, "Skew Y", false },
        { kGodRaysParamCenter, "Center", false },
        { kGodRaysParamFromColor, "From Color", false },
        { kGodRaysParamToColor, "To Color", false },
        { kGodRaysParamGamma, "Gamma", false },
        { kGodRaysParamSamples, "Samples", false },
        { kGodRaysParamMax, "Max", false },
        { kResamplerParamFilterType, "Filter", false },
        { kResamplerParamFilterClamp, "Clamp", false },
        { kResamplerParamFilterBlackOutside, "Black outside", false },
        // Only the Viewer's transform handle uses these.
        { kGodRaysParamUniform, "Uniform", true },
        { kGodRaysParamSkewOrder, "Skew Order", true },
    };
    for (const auto& k : knobs) {
        KnobIPtr knob = node->getKnobByName(k.name);
        ASSERT_TRUE(bool(knob)) << k.name;
        EXPECT_EQ(std::string(k.label), knob->getLabel()) << k.name;
        EXPECT_EQ(k.secret, knob->getIsSecret()) << k.name;
    }
    // Nothing beyond the transform, the tints, Gamma, Samples, Max and the resampling.
    for (const char* gone : { "transformAmount", "invert", "interactive", "resetCenter", "ramp", "falloff", "length", "steps" }) {
        EXPECT_FALSE(bool(node->getKnobByName(gone))) << gone;
    }

    KnobDoublePtr translate = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kGodRaysParamTranslate));
    ASSERT_TRUE(bool(translate));
    EXPECT_EQ(0., translate->getValue(0));
    EXPECT_EQ(0., translate->getValue(1));
    KnobDoublePtr scale = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kGodRaysParamScale));
    ASSERT_TRUE(bool(scale));
    EXPECT_EQ(1., scale->getValue(0));
    EXPECT_EQ(1., scale->getValue(1));
    KnobDoublePtr center = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kGodRaysParamCenter));
    ASSERT_TRUE(bool(center));
    EXPECT_DOUBLE_EQ(kCenterX, center->getValue(0));
    EXPECT_DOUBLE_EQ(kCenterY, center->getValue(1));
    KnobColorPtr from = std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kGodRaysParamFromColor));
    ASSERT_TRUE(bool(from));
    EXPECT_EQ(4, from->getDimension());
    KnobColorPtr to = std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kGodRaysParamToColor));
    ASSERT_TRUE(bool(to));
    EXPECT_EQ(4, to->getDimension());
    for (int d = 0; d < 4; ++d) {
        EXPECT_EQ(1., from->getValue(d));
        EXPECT_EQ(1., to->getValue(d));
    }
    KnobDoublePtr gamma = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kGodRaysParamGamma));
    ASSERT_TRUE(bool(gamma));
    EXPECT_EQ(1., gamma->getValue());
    KnobIntPtr samples = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kGodRaysParamSamples));
    ASSERT_TRUE(bool(samples));
    EXPECT_EQ(32, samples->getValue());
    EXPECT_EQ(GodRays::kMaxSamples, samples->getMaximum());
    KnobBoolPtr max = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kGodRaysParamMax));
    ASSERT_TRUE(bool(max));
    EXPECT_FALSE(max->getValue());

    // The Transform node's filter knobs and defaults; Clamp is enabled only for a filter that honours it.
    KnobChoicePtr filter = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kResamplerParamFilterType));
    ASSERT_TRUE(bool(filter));
    EXPECT_EQ((int)Resampler::eFilterCubic, filter->getValue());
    KnobBoolPtr clamp = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kResamplerParamFilterClamp));
    ASSERT_TRUE(bool(clamp));
    EXPECT_FALSE(clamp->isEnabled(0));
    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("keys")));
    EXPECT_TRUE(clamp->isEnabled(0));
    KnobBoolPtr blackOutside = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kResamplerParamFilterBlackOutside));
    ASSERT_TRUE(bool(blackOutside));
    EXPECT_TRUE(blackOutside->getValue());
}

// The copies run from none of the transform to all of it; averaging, the factors of white tints
// sum to one; Gamma bends the ramp from From Color to To Color.
TEST_F(NativeGodRaysTest, SamplesAndTints)
{
    GodRaysParams p;
    p.samples = 5;
    const std::vector<double> a = GodRays::samplePositions(p);
    ASSERT_EQ(5u, a.size());
    for (int i = 0; i < 5; ++i) {
        EXPECT_DOUBLE_EQ(i / 4., a[i]);
    }
    p.samples = 1;
    ASSERT_EQ(1u, GodRays::samplePositions(p).size());
    EXPECT_EQ(0., GodRays::samplePositions(p)[0]);

    p.samples = 7;
    p.transform.scaleX = p.transform.scaleY = 1.5;
    double sum = 0.;
    const std::vector<DirBlurSample> s = GodRays::canonicalSamples(p);
    ASSERT_EQ(7u, s.size());
    EXPECT_TRUE(s.front().inverse.isIdentity());
    for (const DirBlurSample& sample : s) {
        sum += sample.factor[0];
        EXPECT_FLOAT_EQ(1.f / 7.f, sample.weight);
    }
    EXPECT_NEAR(1., sum, 1e-6);
    // The last copy takes the whole transform: a pixel at the centre plus 3 reads 2 from it.
    const TransformMath::Point3 q = s.back().inverse * TransformMath::Point3(p.transform.centerX + 3., p.transform.centerY, 1.);
    EXPECT_NEAR(p.transform.centerX + 2., q.x / q.z, 1e-9);

    // With Max each copy counts its whole tint.
    p.max = true;
    for (const DirBlurSample& sample : GodRays::canonicalSamples(p)) {
        EXPECT_FLOAT_EQ(1.f, sample.factor[0]);
    }

    p.fromColor[0] = 2.;
    p.toColor[0] = 0.;
    EXPECT_DOUBLE_EQ(2., GodRays::tintAt(p, 0, 0.));
    EXPECT_DOUBLE_EQ(1., GodRays::tintAt(p, 0, 0.5));
    EXPECT_DOUBLE_EQ(0., GodRays::tintAt(p, 0, 1.));
    p.gamma = 2.;
    EXPECT_DOUBLE_EQ(2. - 2. * std::sqrt(0.25), GodRays::tintAt(p, 0, 0.25));
    EXPECT_DOUBLE_EQ(0., GodRays::tintAt(p, 0, 1.));
    p.gamma = 0.5;
    EXPECT_DOUBLE_EQ(2. - 2. * 0.0625, GodRays::tintAt(p, 0, 0.25));
}

TEST_F(NativeGodRaysTest, DefaultsLeaveTheImageUnchanged)
{
    resetProject();
    NodePtr source = makeParitySource(0, 0);
    NodePtr node = makeGodRays(source);
    ASSERT_TRUE(bool(source) && bool(node));
    EXPECT_TRUE(GodRays::isIdentity(dynamic_cast<GodRays*>(node->getEffectInstance().get())->getParams(kTime, ViewIdx(0))));
    const RectI window(0, 0, kWidth, kHeight);
    RenderedPlane in, out;
    ASSERT_TRUE(renderRGBA(source, window, &in));
    ASSERT_TRUE(renderRGBA(node, window, &out));
    EXPECT_EQ(0.f, maxDifference(in, out));

    // A tint alone, untransformed, is not an identity.
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamToColor, { 0.5, 0.5, 0.5, 1. }));
    EXPECT_FALSE(GodRays::isIdentity(dynamic_cast<GodRays*>(node->getEffectInstance().get())->getParams(kTime, ViewIdx(0))));
}

// A whole-pixel translation averages copies moved by 0, 1, ... pixels: the copies are exact
// under the bilinear filter, so the result is the mean of the shifted source.
TEST_F(NativeGodRaysTest, TranslateAveragesShiftedCopies)
{
    resetProject();
    NodePtr source = makeParitySource(0, 0);
    NodePtr node = makeGodRays(source);
    ASSERT_TRUE(bool(source) && bool(node));
    const int n = 9;
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamTranslate, { (double)(n - 1), 0. }));
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamSamples, { (double)n }));

    // The region of definition grows by the translation.
    const RectI rod = pixelRoD(node);
    EXPECT_GE(rod.x2, kWidth + n - 1);

    const RectI window(n, 6, kWidth - 4, kHeight - 6);
    const RectI srcWindow(0, 6, kWidth - 4, kHeight - 6);
    RenderedPlane in, out, peak;
    ASSERT_TRUE(renderRGBA(source, srcWindow, &in));
    ASSERT_TRUE(renderRGBA(node, window, &out));
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamMax, { 1. }));
    ASSERT_TRUE(renderRGBA(node, window, &peak));
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            for (int c = 0; c < 4; ++c) {
                double mean = 0.;
                float best = 0.f;
                for (int i = 0; i < n; ++i) {
                    const float v = at(in, x - i, y, c);
                    mean += v;
                    best = std::max(best, v);
                }
                mean /= n;
                ASSERT_NEAR(mean, at(out, x, y, c), 1e-5) << "(" << x << "," << y << ") channel " << c;
                ASSERT_NEAR(best, at(peak, x, y, c), 1e-6) << "max at (" << x << "," << y << ") channel " << c;
            }
        }
    }
}

// Untransformed copies of a flat field take the mean tint along the rays.
TEST_F(NativeGodRaysTest, TintsAFlatField)
{
    resetProject();
    NodePtr source = makeConstant(0.8);
    NodePtr node = makeGodRays(source);
    ASSERT_TRUE(bool(source) && bool(node));
    const int n = 5;
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamSamples, { (double)n }));
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamFromColor, { 1., 0.5, 1., 1. }));
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamToColor, { 0., 0.5, 0.2, 1. }));
    for (double gamma : { 1., 2., 0.5 }) {
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamGamma, { gamma }));
        GodRaysParams p = dynamic_cast<GodRays*>(node->getEffectInstance().get())->getParams(kTime, ViewIdx(0));
        RenderedPlane out;
        ASSERT_TRUE(renderRGBA(node, RectI(20, 14, 44, 34), &out));
        for (int c = 0; c < 3; ++c) {
            double mean = 0.;
            for (double a : GodRays::samplePositions(p)) {
                mean += GodRays::tintAt(p, c, a);
            }
            mean /= n;
            EXPECT_NEAR(0.8 * mean, at(out, 32, 24, c), 1e-5) << "gamma " << gamma << ", channel " << c;
        }
        EXPECT_NEAR(1., at(out, 32, 24, 3), 1e-5) << "gamma " << gamma;
    }
}

// Scaling up about Center streaks the image away from it: the region of definition grows about
// Center, and a pixel beyond the source's edge on the far side of Center picks up the rays.
TEST_F(NativeGodRaysTest, ScaleStreaksAwayFromCenter)
{
    resetProject();
    const RectI box(24, 16, 40, 32);
    NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { (double)box.x1, (double)box.y1 }));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)box.width(), (double)box.height() }));
    ASSERT_TRUE(setKnobValues(source, kConstantParamColor, { 1., 1., 1., 1. }));
    NodePtr node = makeGodRays(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamScale, { 2., 2. }));

    const RectI rod = pixelRoD(node);
    EXPECT_LE(rod.x1, 16);
    EXPECT_GE(rod.x2, 48);
    EXPECT_LE(rod.y1, 8);
    EXPECT_GE(rod.y2, 40);

    RenderedPlane out;
    ASSERT_TRUE(renderRGBA(node, rod, &out));
    // Outside the box, on the rays, some but not all copies land.
    EXPECT_GT(at(out, 44, 24, 0), 0.f);
    EXPECT_LT(at(out, 44, 24, 0), 1.f);
    // At Center every copy is the box's.
    EXPECT_NEAR(1., at(out, 32, 24, 0), 1e-5);
}

// A tile of the render equals the same pixels of the full render, bit for bit.
TEST_F(NativeGodRaysTest, TileMatchesFullFrame)
{
    resetProject();
    NodePtr source = makeParitySource(0, 0);
    ASSERT_TRUE(bool(source));
    const struct {
        const char* filter;
        bool clamp;
        bool blackOutside;
        bool max;
    } cases[] = {
        { "bilinear", false, true, false },
        { "cubic", false, true, true },
        { "keys", true, false, false },
        { "impulse", false, false, true },
    };
    for (const auto& tc : cases) {
        NodePtr node = makeGodRays(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string(tc.filter)));
        ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { tc.clamp ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { tc.blackOutside ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamTranslate, { 3., -2. }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamRotate, { 12. }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamScale, { 1.3, 1.1 }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamSkewX, { 0.1 }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamSamples, { 9. }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamToColor, { 0.5, 0.8, 1., 1. }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamGamma, { 1.7 }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamMax, { tc.max ? 1. : 0. }));
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
                        ASSERT_EQ(at(whole, x, y, c), at(part, x, y, c)) << tc.filter << (tc.clamp ? " clamped" : "") << (tc.blackOutside ? "" : " edge repeat") << (tc.max ? " max" : "") << " at (" << x << "," << y << ") channel " << c;
                    }
                }
            }
        }
    }
}

TEST_F(NativeGodRaysTest, SameBothWays)
{
    resetProject();
    NodePtr source = makeParitySource(0, 0);
    NodePtr node = makeGodRays(source);
    ASSERT_TRUE(bool(source) && bool(node));
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamScale, { 1.4, 1.4 }));

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
    expectSame("scale");
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamRotate, { 20. }));
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamTranslate, { 5., 3. }));
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamToColor, { 0.2, 0.4, 1., 0.5 }));
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamGamma, { 2. }));
    expectSame("transform-tint-gamma");
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamMax, { 1. }));
    expectSame("max");
    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("keys")));
    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { 0. }));
    expectSame("keys-clamp-edge-repeat");

    NodePtr mask = makeParitySource(8, 4);
    ASSERT_TRUE(bool(mask));
    connectNodes(mask, node, kGodRaysInputMask, true);
    ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectSame("mask-mix");
}

// Regression fixtures: rays from a scale and a rotation, tinted, with Gamma and Max, and through a
// mask and mix.
TEST_F(NativeGodRaysTest, Golden)
{
    GoldenNodeSpec spec;
    spec.name = "GodRays";
    spec.pluginID = PLUGINID_NATRON_GODRAYS;
    spec.major = PLUGIN_MAJOR_NATRON_GODRAYS;
    spec.maskInput = "Mask";

    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("scale", false, [](const GoldenGraph& g) {
        return setKnobValues(g.node, kGodRaysParamScale, { 1.4, 1.4 });
    }));
    v.push_back(goldenVariant("transformTint", false, [](const GoldenGraph& g) {
        return setKnobValues(g.node, kGodRaysParamScale, { 1.3, 1.1 }) && setKnobValues(g.node, kGodRaysParamRotate, { 8. }) && setKnobValues(g.node, kGodRaysParamTranslate, { 4., -2. }) && setKnobValues(g.node, kGodRaysParamFromColor, { 1., 0.9, 0.6, 1. }) && setKnobValues(g.node, kGodRaysParamToColor, { 0.1, 0.2, 0.8, 0.5 });
    }));
    v.push_back(goldenVariant("gammaMax", false, [](const GoldenGraph& g) {
        return setKnobValues(g.node, kGodRaysParamScale, { 1.5, 1.5 }) && setKnobValues(g.node, kGodRaysParamToColor, { 0., 0., 0., 0. }) && setKnobValues(g.node, kGodRaysParamGamma, { 2.5 }) && setKnobValues(g.node, kGodRaysParamMax, { 1. });
    }));
    v.push_back(goldenVariant("keysClampEdgeRepeat", false, [](const GoldenGraph& g) {
        return setKnobValues(g.node, kGodRaysParamScale, { 1.25, 1.25 }) && setKnobValues(g.node, kGodRaysParamSkewX, { 0.2 }) && setKnobValue(g.node, kResamplerParamFilterType, std::string("keys")) && setKnobValues(g.node, kResamplerParamFilterClamp, { 1. }) && setKnobValues(g.node, kResamplerParamFilterBlackOutside, { 0. });
    }));
    v.push_back(goldenVariant("maskmix", true, [](const GoldenGraph& g) {
        return setKnobValues(g.node, kGodRaysParamScale, { 1.4, 1.4 }) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}
