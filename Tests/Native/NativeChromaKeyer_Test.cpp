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
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Keyer/ChromaKeyer.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kChromaKeyerID = PLUGINID_NATRON_CHROMAKEYER;
const int kOfxChromaKeyerMajor = 1;
const int kNativeChromaKeyerMajor = PLUGIN_MAJOR_NATRON_CHROMAKEYER;
const double kTime = 1.;

// The maths is single-precision conversions around double-precision key arithmetic, with a
// Rec. 709 transfer function in front, the class the plan tabulates for it.
const ParityTolerance kChromaKeyerTolerance = ParityTolerance::transcendental();

// The OpenFX plug-in is built with -Ofast, so its key differs from this node's by a few ulps, and
// the unpremultiplied output divides the suppressed colour by a key alpha that can be near zero,
// magnifying those ulps: measured under 5e-5 relative with masks, and under 1e-4 at the angle extremes.
const ParityTolerance kChromaKeyerUnpremultipliedTolerance = ParityTolerance::make(0., 1e-4);

const ParityTolerance&
toleranceForShow(const std::string& show)
{
    return (show == "unpremultiplied") ? kChromaKeyerUnpremultipliedTolerance : kChromaKeyerTolerance;
}

const double kGreenKey[3] = { 0.1, 0.8, 0.2 };

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

bool
setChannelSelect(const NodePtr& node,
                 const std::string& name,
                 const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(select != NULL) << name;
    if (!select) {
        return false;
    }
    select->set(value);

    return true;
}

bool
setChannelSelectOnBoth(const ParityPair& pair,
                       const std::string& name,
                       const std::string& value)
{
    return setChannelSelect(pair.native, name, value) && (!pair.ofx || setChannelSelect(pair.ofx, name, value));
}

} // namespace

class NativeChromaKeyerTest
    : public BaseTest {
protected:
    void resetProject()
    {
        getApp()->getProject()->reset(false, true);
    }

    // The pair on a source of `components`, with Bg and the two masks connected on request, each
    // at its own origin so that the regions of definition differ.
    ParityPair makePair(bool withBg,
                        bool withMasks,
                        const std::string& components = std::string("rgba"))
    {
        resetProject();
        ParityPair pair = makeParityPair(getApp(), kChromaKeyerID, kOfxChromaKeyerMajor, kNativeChromaKeyerMajor);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_FALSE(pair.live()) << "the OFX ChromaKeyer is retired, so parity replays the recorded references";
        EXPECT_TRUE(isNative(pair.native));
        if (!pair.native || !pair.source) {
            return pair;
        }
        setParitySourceComponents(pair.source, components);
        if (withBg) {
            NodePtr bg = connectParityInput(pair, "Bg");
            EXPECT_TRUE(bool(bg));
            if (bg) {
                setParitySourceOrigin(bg, 8, 4);
            }
        }
        if (withMasks) {
            const char* const labels[2] = { "InM", "OutM" };
            const int origins[2][2] = { { -6, 10 }, { 12, 6 } };
            for (int k = 0; k < 2; ++k) {
                NodePtr mask = connectParityInput(pair, labels[k]);
                EXPECT_TRUE(bool(mask)) << labels[k];
                if (!mask) {
                    continue;
                }
                setParitySourceOrigin(mask, origins[k][0], origins[k][1]);
                EXPECT_TRUE(setKnobOnBoth(pair, std::string("enableMask_") + labels[k], { 1. }));
                EXPECT_TRUE(setChannelSelectOnBoth(pair, std::string("maskChannel_") + labels[k], "rgba.A"));
            }
        }

        return pair;
    }

    void setKey(const ParityPair& pair,
                const double key[3])
    {
        ASSERT_TRUE(setKnobOnBoth(pair, kChromaKeyerParamKeyColor, { key[0], key[1], key[2] }));
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record = false,
                      const ParityTolerance& tolerance = kChromaKeyerTolerance)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, tolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] ChromaKeyer " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }
};

TEST_F(NativeChromaKeyerTest, InputsKeepTheOfxOrder)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kChromaKeyerID), kNativeChromaKeyerMajor);
    ASSERT_TRUE(isNative(node));

    EffectInstancePtr effect = node->getEffectInstance();
    ASSERT_EQ(4, effect->getNInputs());
    EXPECT_EQ("Source", effect->getInputLabel(kChromaKeyerInputSource));
    EXPECT_EQ("InM", effect->getInputLabel(kChromaKeyerInputInsideMask));
    EXPECT_EQ("OutM", effect->getInputLabel(kChromaKeyerInputOutsideMask));
    EXPECT_EQ("Bg", effect->getInputLabel(kChromaKeyerInputBg));
    EXPECT_FALSE(effect->isInputOptional(kChromaKeyerInputSource));
    EXPECT_TRUE(effect->isInputOptional(kChromaKeyerInputBg));
    EXPECT_FALSE(effect->isInputMask(kChromaKeyerInputSource));
    EXPECT_TRUE(effect->isInputMask(kChromaKeyerInputInsideMask));
    EXPECT_TRUE(effect->isInputMask(kChromaKeyerInputOutsideMask));
    EXPECT_FALSE(effect->isInputMask(kChromaKeyerInputBg));
}

TEST_F(NativeChromaKeyerTest, UnversionedRequestsGetTheNativeChromaKeyer)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kChromaKeyerID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeChromaKeyerMajor, unversioned->getMajorVersion());
}

TEST_F(NativeChromaKeyerTest, CompositeOverBg)
{
    ParityPair pair = makePair(true, false);
    ASSERT_TRUE(bool(pair.native));
    setKey(pair, kGreenKey);
    expectParity(pair, "composite-green", true);
}

TEST_F(NativeChromaKeyerTest, PremultipliedWithMasks)
{
    ParityPair pair = makePair(true, true);
    ASSERT_TRUE(bool(pair.native));
    setKey(pair, kGreenKey);
    ASSERT_TRUE(setKnobOnBoth(pair, kChromaKeyerParamShow, std::string("premultiplied")));
    expectParity(pair, "masks-premultiplied", true, toleranceForShow("premultiplied"));
}

TEST_F(NativeChromaKeyerTest, UnpremultipliedRgbSource)
{
    ParityPair pair = makePair(true, true, "rgb");
    ASSERT_TRUE(bool(pair.native));
    setKey(pair, kGreenKey);
    ASSERT_TRUE(setKnobOnBoth(pair, kChromaKeyerParamShow, std::string("unpremultiplied")));
    expectParity(pair, "rgb-source-unpremultiplied", true, toleranceForShow("unpremultiplied"));
}

TEST_F(NativeChromaKeyerTest, BothSchedulerModesAgreeWithNoUnplannedPull)
{
    ParityPair pair = makePair(true, true);
    ASSERT_TRUE(bool(pair.native));
    setKey(pair, kGreenKey);
    ASSERT_TRUE(setKnobValue(pair.native, kChromaKeyerParamSourceAlpha, "normal"));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RectI window = paritySourceWindow(pair.source, kTime, mipmapLevel);
        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(pair.native, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
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

void
setColorKnob(const NodePtr& node,
             const char* name,
             const double* values)
{
    KnobColor* color = dynamic_cast<KnobColor*>(node->getKnobByName(name).get());

    ASSERT_TRUE(color != NULL) << name;
    for (int i = 0; i < color->getDimension(); ++i) {
        color->setValue(values[i], ViewSpec::all(), i, eValueChangedReasonPluginEdited, NULL);
    }
}
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

    KnobBoolPtr convertNaNs = std::dynamic_pointer_cast<KnobBool>(appPTR->getCurrentSettings()->getKnobByName("convertNaNs"));
    ASSERT_TRUE(bool(convertNaNs));
    const bool convertNaNsWas = convertNaNs->getValue();
    convertNaNs->setValue(false);

    resetProject();
    NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr keyer = createNode(QString::fromUtf8(kChromaKeyerID));
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(keyer));
    connectNodes(source, keyer, 0, true);

    const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    for (const ProblemPixel& pixel : pixels) {
        setColorKnob(source, kConstantParamColor, pixel.rgba);
        for (const double* key : keys) {
            setColorKnob(keyer, kChromaKeyerParamKeyColor, key);
            for (int show = 0; show < 4; ++show) {
                for (int sourceAlpha = 0; sourceAlpha < 3; ++sourceAlpha) {
                    for (int linear = 0; linear < 2; ++linear) {
                        ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamShow, { (double)show }));
                        ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamSourceAlpha, { (double)sourceAlpha }));
                        ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamLinear, { (double)linear }));
                        std::vector<RenderedPlane> planes;
                        std::string error;
                        ASSERT_TRUE(renderNodePlanesDirect(keyer, 1., ViewIdx(0), 0, RectI(0, 0, 4, 4), layers, &planes, &error)) << error;
                        ASSERT_EQ(1u, planes.size());
                        const std::string what = std::string(pixel.name) + ", key (" + std::to_string(key[0]) + ", " + std::to_string(key[1]) + ", " + std::to_string(key[2])
                            + "), show " + std::to_string(show) + ", source alpha " + std::to_string(sourceAlpha) + ", linear " + std::to_string(linear);
                        for (std::size_t i = 0; i < planes[0].pixels.size(); ++i) {
                            ASSERT_FALSE(std::isnan(planes[0].pixels[i])) << what << ", value " << i;
                        }
                    }
                }
            }
        }
    }
    convertNaNs->setValue(convertNaNsWas);
}
