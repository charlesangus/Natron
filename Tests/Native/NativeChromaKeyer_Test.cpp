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
#include <limits>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

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
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Keyer/ChromaKeyer.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kChromaKeyerID = PLUGINID_NATRON_CHROMAKEYER;
const double kTime = 1.;

const double kGreenKey[3] = { 0.1, 0.8, 0.2 };

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

// The node keys in float around double arithmetic and the expectations are computed in double.
double
tolerance(double expected)
{
    return 1e-4 * (std::max)(1., std::abs(expected));
}

struct KeyResult {
    double rgb[3];
    double alpha;
};

// The key with the default angles (acceptance 120, suppression 40), lift 0 and gain 1, the
// source alpha ignored, in double: the expectation every analytic case below compares against.
KeyResult
referenceKey(const double w[3],
             const double key[3],
             const double fg[3],
             bool suppress)
{
    const double kr = w[0];
    const double kb = w[2];
    const double kg = 1. - kr - kb;
    const auto toYPbPr = [&](const double* c, double* y, double* pb, double* pr) {
        *y = kr * c[0] + kg * c[1] + kb * c[2];
        *pb = (c[2] - *y) / (2. * (1. - kb));
        *pr = (c[0] - *y) / (2. * (1. - kr));
    };
    double ky, kcb, kcr;
    toYPbPr(key, &ky, &kcb, &kcr);
    const double xKey = 2. * std::sqrt(kcb * kcb + kcr * kcr);
    const double cosKey = 2. * kcb / xKey;
    const double sinKey = 2. * kcr / xKey;
    const double ys = ky / xKey;
    const double tanAcceptance = std::tan(60. * M_PI / 180.);
    const double tanSuppression = std::tan(20. * M_PI / 180.);

    double y, cb, cr;
    toYPbPr(fg, &y, &cb, &cr);
    const double x = cosKey * 2. * cb + sinKey * 2. * cr;
    const double z = std::abs(-sinKey * 2. * cb + cosKey * 2. * cr);
    const double kfg = ((x <= 0.) || (z / x > tanAcceptance)) ? 0. : x - z / tanAcceptance;

    KeyResult r;
    r.alpha = 1. - kfg / xKey;
    for (int c = 0; c < 3; ++c) {
        r.rgb[c] = fg[c];
    }
    if (suppress) {
        if ((x > 0.) && (x - z / tanSuppression > 0.)) {
            cb = cr = 0.;
        } else {
            cb -= kfg * cosKey / 2.;
            cr -= kfg * sinKey / 2.;
        }
        y -= ys * kfg;
        r.rgb[2] = cb * 2. * (1. - kb) + y;
        r.rgb[0] = cr * 2. * (1. - kr) + y;
        r.rgb[1] = (y - kr * r.rgb[0] - kb * r.rgb[2]) / kg;
    }

    return r;
}

bool
renderRgba(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return !plane->pixels.empty();
}

void
expectPixel(const RenderedPlane& plane,
            const double rgb[3],
            double alpha,
            const std::string& what)
{
    ASSERT_GE(plane.pixels.size(), 4u) << what;
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(rgb[c], plane.pixels[c], tolerance(rgb[c])) << what << ", channel " << c;
    }
    EXPECT_NEAR(alpha, plane.pixels[3], tolerance(alpha)) << what << ", alpha";
}

} // namespace

class NativeChromaKeyerTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    NodePtr makeKeyer(const NodePtr& source)
    {
        NodePtr keyer = createNode(QString::fromUtf8(kChromaKeyerID), PLUGIN_MAJOR_NATRON_CHROMAKEYER);

        EXPECT_TRUE(isNative(keyer));
        if (keyer && source) {
            connectNodes(source, keyer, kChromaKeyerInputSource, true);
        }
        if (keyer) {
            EXPECT_TRUE(setKnobValues(keyer, kChromaKeyerParamKeyColor, std::vector<double>(kGreenKey, kGreenKey + 3)));
        }

        return keyer;
    }

    NodePtr makeConstant(const std::vector<double>& rgba)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);

        EXPECT_TRUE(bool(constant));
        if (constant) {
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, rgba));
        }

        return constant;
    }

    void workingLuma(double w[3])
    {
        ProjectColorManagementPtr cm = getApp()->getProject()->getColorManagement();

        ASSERT_TRUE(bool(cm));
        cm->getWorkingLuma(w);
    }

    void setWorkingSpace(const std::string& name)
    {
        KnobChoicePtr workingSpace = getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");
        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID(name, 0);
        ASSERT_EQ(name, getApp()->getProject()->getWorkingColorSpace());
    }
};

// Source, Bg and the host's Mask, with the host footer; the colourspace choice, the Rec. 709
// toggle and the inside and outside masks are gone.
TEST_F(NativeChromaKeyerTest, ControlsAndInputs)
{
    NodePtr node = makeKeyer(NodePtr());
    ASSERT_TRUE(bool(node));

    EffectInstancePtr effect = node->getEffectInstance();
    ASSERT_EQ(3, effect->getNInputs());
    EXPECT_EQ("Source", effect->getInputLabel(kChromaKeyerInputSource));
    EXPECT_EQ("Bg", effect->getInputLabel(kChromaKeyerInputBg));
    EXPECT_EQ("Mask", effect->getInputLabel(kChromaKeyerInputMask));
    EXPECT_FALSE(effect->isInputOptional(kChromaKeyerInputSource));
    EXPECT_TRUE(effect->isInputOptional(kChromaKeyerInputBg));
    EXPECT_FALSE(effect->isInputMask(kChromaKeyerInputBg));
    EXPECT_TRUE(effect->isInputMask(kChromaKeyerInputMask));

    EXPECT_FALSE(bool(node->getKnobByName("colorspace")));
    EXPECT_FALSE(bool(node->getKnobByName("linearProcessing")));
    EXPECT_FALSE(bool(node->getKnobByName("enableMask_InM")));
    EXPECT_FALSE(bool(node->getKnobByName("enableMask_OutM")));
    EXPECT_TRUE(bool(node->getKnobByName(kOfxMixParamName)));
    EXPECT_TRUE(bool(node->getKnobByName(kOfxMaskInvertParamName)));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));

    const char* const kept[] = { kChromaKeyerParamKeyColor, kChromaKeyerParamAcceptanceAngle, kChromaKeyerParamSuppressionAngle, kChromaKeyerParamKeyLift,
                                 kChromaKeyerParamKeyGain, kChromaKeyerParamShow, kChromaKeyerParamSourceAlpha };
    for (const char* name : kept) {
        EXPECT_TRUE(bool(node->getKnobByName(name))) << name;
    }
    KnobChoicePtr show = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kChromaKeyerParamShow));
    ASSERT_TRUE(bool(show));
    EXPECT_EQ("composite", show->getEntry(show->getValue()).id);
}

TEST_F(NativeChromaKeyerTest, UnversionedRequestsGetTheNativeChromaKeyer)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kChromaKeyerID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_CHROMAKEYER, unversioned->getMajorVersion());
}

TEST_F(NativeChromaKeyerTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_CHROMAKEYER, PLUGIN_MAJOR_NATRON_CHROMAKEYER, "ChromaKeyer");
}

// A green spill above 1 with a negative blue keeps both after suppression: nothing clamps the
// foreground to [0, 1], and the key alpha follows the ramp below 0.
TEST_F(NativeChromaKeyerTest, HdrForegroundIsNotClipped)
{
    const double fg[4] = { 1.5, 3., 0.2, 1. };
    NodePtr source = makeConstant(std::vector<double>(fg, fg + 4));
    NodePtr keyer = makeKeyer(source);
    ASSERT_TRUE(bool(source) && bool(keyer));
    ASSERT_TRUE(setKnobValue(keyer, kChromaKeyerParamShow, "premultiplied"));
    double w[3];
    workingLuma(w);

    const KeyResult expected = referenceKey(w, kGreenKey, fg, true);
    EXPECT_GT(expected.rgb[1], 1.);
    EXPECT_LT(expected.rgb[2], 0.);
    EXPECT_LT(expected.alpha, 0.);
    RenderedPlane out;
    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
    expectPixel(out, expected.rgb, expected.alpha, "HDR green spill");

    // A magenta far outside the acceptance angle comes through whole, above 1 and below 0.
    const double magenta[4] = { 4., -0.2, 3., 1. };
    ASSERT_TRUE(setKnobValues(source, kConstantParamColor, std::vector<double>(magenta, magenta + 4)));
    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
    expectPixel(out, magenta, 1., "magenta");
}

// The key colour itself keys out completely and suppresses to black; twice as bright it keys
// past the background, with no clamp on the key.
TEST_F(NativeChromaKeyerTest, BackgroundKeyIsNotClamped)
{
    const double black[3] = { 0., 0., 0. };
    NodePtr source = makeConstant({ kGreenKey[0], kGreenKey[1], kGreenKey[2], 1. });
    NodePtr keyer = makeKeyer(source);
    ASSERT_TRUE(bool(source) && bool(keyer));
    ASSERT_TRUE(setKnobValue(keyer, kChromaKeyerParamShow, "premultiplied"));

    RenderedPlane out;
    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
    expectPixel(out, black, 0., "the key colour");

    ASSERT_TRUE(setKnobValues(source, kConstantParamColor, { 2. * kGreenKey[0], 2. * kGreenKey[1], 2. * kGreenKey[2], 1. }));
    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
    expectPixel(out, black, -1., "twice the key colour");
}

// In Composite mode the keyed-out key colour shows Bg, scene-linear values and all.
TEST_F(NativeChromaKeyerTest, CompositeShowsBg)
{
    const double bg[4] = { 0.3, 0.4, 2.5, 1. };
    NodePtr source = makeConstant({ kGreenKey[0], kGreenKey[1], kGreenKey[2], 1. });
    NodePtr background = makeConstant(std::vector<double>(bg, bg + 4));
    NodePtr keyer = makeKeyer(source);
    ASSERT_TRUE(bool(source) && bool(background) && bool(keyer));
    connectNodes(background, keyer, kChromaKeyerInputBg, true);

    RenderedPlane out;
    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
    expectPixel(out, bg, 0., "the key colour over Bg");
}

// "Inside Mask" source alpha holds the key at one minus the source alpha.
TEST_F(NativeChromaKeyerTest, InsideMaskSourceAlphaKeepsTheForeground)
{
    NodePtr source = makeConstant({ kGreenKey[0], kGreenKey[1], kGreenKey[2], 0.75 });
    NodePtr keyer = makeKeyer(source);
    ASSERT_TRUE(bool(source) && bool(keyer));
    ASSERT_TRUE(setKnobValue(keyer, kChromaKeyerParamSourceAlpha, "insidemask"));
    ASSERT_TRUE(setKnobValue(keyer, kChromaKeyerParamShow, "intermediate"));
    double w[3];
    workingLuma(w);
    const double fg[3] = { kGreenKey[0], kGreenKey[1], kGreenKey[2] };
    // The key colour's background key is its own chroma distance, xKey; held at 0.25 it leaves an
    // alpha of 1 - 0.25 / xKey.
    const KeyResult unheld = referenceKey(w, kGreenKey, fg, false);
    ASSERT_NEAR(0., unheld.alpha, 1e-12);
    double ky, kcb, kcr;
    {
        const double kr = w[0];
        const double kb = w[2];
        ky = kr * fg[0] + (1. - kr - kb) * fg[1] + kb * fg[2];
        kcb = (fg[2] - ky) / (2. * (1. - kb));
        kcr = (fg[0] - ky) / (2. * (1. - kr));
    }
    const double xKey = 2. * std::sqrt(kcb * kcb + kcr * kcr);

    RenderedPlane out;
    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
    expectPixel(out, fg, 1. - 0.25 / xKey, "held by the source alpha");
}

// The host mask and mix blend the keyed result against the Source.
TEST_F(NativeChromaKeyerTest, HostMaskAndMixLimitTheKey)
{
    const double fg[4] = { 1.5, 3., 0.2, 1. };
    NodePtr source = makeConstant(std::vector<double>(fg, fg + 4));
    NodePtr mask = makeConstant({ 0., 0., 0., 0.25 });
    NodePtr keyer = makeKeyer(source);
    ASSERT_TRUE(bool(source) && bool(mask) && bool(keyer));
    ASSERT_TRUE(setKnobValue(keyer, kChromaKeyerParamShow, "premultiplied"));
    connectNodes(mask, keyer, kChromaKeyerInputMask, true);
    KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(keyer->getKnobByName("maskChannel_Mask").get());
    ASSERT_TRUE(maskChannel != NULL);
    maskChannel->set("rgba.A");
    ASSERT_TRUE(setKnobValues(keyer, "enableMask_Mask", std::vector<double>(1, 1.)));
    double w[3];
    workingLuma(w);
    const KeyResult keyed = referenceKey(w, kGreenKey, fg, true);
    const auto blend = [&](double amount, double* rgb, double* alpha) {
        for (int c = 0; c < 3; ++c) {
            rgb[c] = fg[c] + amount * (keyed.rgb[c] - fg[c]);
        }
        *alpha = fg[3] + amount * (keyed.alpha - fg[3]);
    };
    double rgb[3];
    double alpha;
    RenderedPlane out;

    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
    blend(0.25, rgb, &alpha);
    expectPixel(out, rgb, alpha, "mask 0.25");

    ASSERT_TRUE(setKnobValues(keyer, kOfxMixParamName, std::vector<double>(1, 0.5)));
    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
    blend(0.125, rgb, &alpha);
    expectPixel(out, rgb, alpha, "mask 0.25, mix 0.5");

    ASSERT_TRUE(setKnobValues(keyer, kOfxMaskInvertParamName, std::vector<double>(1, 1.)));
    ASSERT_TRUE(setKnobValues(keyer, kOfxMixParamName, std::vector<double>(1, 1.)));
    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
    blend(0.75, rgb, &alpha);
    expectPixel(out, rgb, alpha, "inverted mask");
}

// Under an ACES working space the key uses AP1's luma and chroma weights, which key this pixel
// measurably differently from Rec. 709's.
TEST_F(NativeChromaKeyerTest, ApOneWeightsUnderAces)
{
    const std::string original = getApp()->getProject()->getWorkingColorSpace();
    setWorkingSpace("ACEScg");
    double w[3];
    workingLuma(w);
    EXPECT_NEAR(0.2676722, w[0], 1e-4);
    EXPECT_NEAR(0.6743400, w[1], 1e-4);
    EXPECT_NEAR(0.0579878, w[2], 1e-4);

    const double fg[4] = { 1.5, 3., 0.2, 1. };
    NodePtr source = makeConstant(std::vector<double>(fg, fg + 4));
    NodePtr keyer = makeKeyer(source);
    ASSERT_TRUE(bool(source) && bool(keyer));
    ASSERT_TRUE(setKnobValue(keyer, kChromaKeyerParamShow, "premultiplied"));

    const KeyResult ap1 = referenceKey(w, kGreenKey, fg, true);
    const double rec709[3] = { 0.2126, 0.7152, 0.0722 };
    EXPECT_GT(std::abs(ap1.alpha - referenceKey(rec709, kGreenKey, fg, true).alpha), 0.05);
    RenderedPlane out;
    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
    expectPixel(out, ap1.rgb, ap1.alpha, "AP1");

    setWorkingSpace(original);
}

TEST_F(NativeChromaKeyerTest, BothSchedulerModesAgreeWithNoUnplannedPull)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr bg = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(source) && bool(bg) && bool(mask));
    setParitySourceOrigin(bg, 8, 4);
    setParitySourceOrigin(mask, -6, 10);
    NodePtr keyer = makeKeyer(source);
    ASSERT_TRUE(bool(keyer));
    connectNodes(bg, keyer, kChromaKeyerInputBg, true);
    connectNodes(mask, keyer, kChromaKeyerInputMask, true);
    ASSERT_TRUE(setKnobValues(keyer, "enableMask_Mask", std::vector<double>(1, 1.)));
    ASSERT_TRUE(setKnobValue(keyer, kChromaKeyerParamSourceAlpha, "normal"));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RectI window = paritySourceWindow(source, kTime, mipmapLevel);
        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(keyer, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        ASSERT_EQ(poolSizes.size(), unplannedPulls.size());
        for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
            EXPECT_EQ(0, unplannedPulls[i]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[i];
        }
    }
}

namespace {
struct ProblemPixel {
    const char* name;
    double rgba[4];
};
} // namespace

// Pixels a real plate or an upstream node can hand the keyer, each keyed against keys that make
// the kernel divide by a small or overflowing chrominance. The host's NaN conversion is off, so
// a NaN the kernel produces reaches the rendered image instead of being replaced by 1.
TEST_F(NativeChromaKeyerTest, ProblemPixelsKeyToFiniteValues)
{
    const double big = std::numeric_limits<float>::max();
    const double inf = std::numeric_limits<double>::infinity();
    const ProblemPixel pixels[] = {
        { "grey", { 0.18, 0.18, 0.18, 1. } },
        { "black-transparent", { 0., 0., 0., 0. } },
        { "negative", { -0.5, 0.2, -0.1, 1. } },
        { "superwhite", { 40., 12., 3., 1. } },
        { "huge", { 1e30, 1e30, 1e30, 1. } },
        { "float-max", { big, big, big, 1. } },
        { "float-max-opposed", { big, 0., -big, 0.5 } },
        { "infinite-red", { inf, 1., 1., 1. } },
        { "superwhite-alpha", { 0.2, 0.8, 0.1, 1e6 } },
    };
    const double keys[][3] = {
        { 0., 0., 0. },
        { 0.1, 0.8, 0.1 },
        { 0.18, 0.18, 0.18 },
        { -0.2, 0.5, 0.1 },
        { 1e-25, 0., 0. },
        { big, 0., 0. },
    };

    NodePtr source = makeConstant({ 0., 0., 0., 1. });
    NodePtr keyer = makeKeyer(source);
    ASSERT_TRUE(bool(source) && bool(keyer));

    for (const ProblemPixel& pixel : pixels) {
        ASSERT_TRUE(setKnobValues(source, kConstantParamColor, std::vector<double>(pixel.rgba, pixel.rgba + 4)));
        for (const double* key : keys) {
            ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamKeyColor, std::vector<double>(key, key + 3)));
            for (int show = 0; show < 4; ++show) {
                for (int sourceAlpha = 0; sourceAlpha < 3; ++sourceAlpha) {
                    ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamShow, { (double)show }));
                    ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamSourceAlpha, { (double)sourceAlpha }));
                    RenderedPlane out;
                    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
                    const std::string what = std::string(pixel.name) + ", key (" + std::to_string(key[0]) + ", " + std::to_string(key[1]) + ", " + std::to_string(key[2])
                        + "), show " + std::to_string(show) + ", source alpha " + std::to_string(sourceAlpha);
                    for (std::size_t i = 0; i < out.pixels.size(); ++i) {
                        ASSERT_FALSE(std::isnan(out.pixels[i])) << what << ", value " << i;
                    }
                }
            }
        }
    }
}
