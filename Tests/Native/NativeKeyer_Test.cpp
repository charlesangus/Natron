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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Keyer/Keyer.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kKeyerID = PLUGINID_NATRON_KEYER;
const int kOfxKeyerMajor = 1;
const int kNativeKeyerMajor = PLUGIN_MAJOR_NATRON_KEYER;
const double kTime = 1.;

// Plain arithmetic in double precision around single-precision key colour sums, the class the
// plan tabulates for it.
const ParityTolerance kKeyerTolerance = ParityTolerance::transcendental();

const double kGreenKey[3] = { 0.1, 0.8, 0.2 };
const char* const kModes[4] = { "luminance", "color", "screen", "none" };
const char* const kShows[4] = { "intermediate", "premultiplied", "unpremultiplied", "composite" };

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

double
knobValue(const NodePtr& node,
          const char* name)
{
    KnobDouble* knob = node ? dynamic_cast<KnobDouble*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(knob != NULL) << name;

    return knob ? knob->getValue(0) : 0.;
}

} // namespace

class NativeKeyerTest
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
        ParityPair pair = makeParityPair(getApp(), kKeyerID, kOfxKeyerMajor, kNativeKeyerMajor);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(pair.live()) << "the OFX Keyer is still loadable, so parity is live";
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

    // The mode first, then the key colour: both edits reset the thresholds, on both nodes alike.
    void setModeAndKey(const ParityPair& pair,
                       const std::string& mode,
                       const double key[3])
    {
        ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamMode, mode));
        ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamKeyColor, { key[0], key[1], key[2] }));
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record = false)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, kKeyerTolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_TRUE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Keyer " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    void expectThresholdsMatch(const ParityPair& pair,
                               const std::string& what)
    {
        const char* const names[5] = { kKeyerParamSoftnessLower, kKeyerParamToleranceLower, kKeyerParamCenter, kKeyerParamToleranceUpper, kKeyerParamSoftnessUpper };

        for (int i = 0; i < 5; ++i) {
            EXPECT_NEAR(knobValue(pair.ofx, names[i]), knobValue(pair.native, names[i]), 1e-6) << what << ": " << names[i];
        }
    }
};

TEST_F(NativeKeyerTest, InputsKeepTheOfxOrder)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kKeyerID), kNativeKeyerMajor);
    ASSERT_TRUE(isNative(node));

    EffectInstancePtr effect = node->getEffectInstance();
    ASSERT_EQ(4, effect->getNInputs());
    EXPECT_EQ("Source", effect->getInputLabel(kKeyerInputSource));
    EXPECT_EQ("InM", effect->getInputLabel(kKeyerInputInsideMask));
    EXPECT_EQ("OutM", effect->getInputLabel(kKeyerInputOutsideMask));
    EXPECT_EQ("Bg", effect->getInputLabel(kKeyerInputBg));
    EXPECT_FALSE(effect->isInputOptional(kKeyerInputSource));
    EXPECT_TRUE(effect->isInputOptional(kKeyerInputBg));
    EXPECT_FALSE(effect->isInputMask(kKeyerInputSource));
    EXPECT_TRUE(effect->isInputMask(kKeyerInputInsideMask));
    EXPECT_TRUE(effect->isInputMask(kKeyerInputOutsideMask));
    EXPECT_FALSE(effect->isInputMask(kKeyerInputBg));
}

TEST_F(NativeKeyerTest, UnversionedRequestsGetTheNativeKeyer)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kKeyerID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeKeyerMajor, unversioned->getMajorVersion());
}

TEST_F(NativeKeyerTest, KnobParity)
{
    ParityPair pair = makePair(false, false);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(bool(pair.ofx));
    expectKnobParity(pair.ofx, pair.native);
}

TEST_F(NativeKeyerTest, DefaultsWithoutBg)
{
    ParityPair pair = makePair(false, false);
    ASSERT_TRUE(bool(pair.native));
    expectParity(pair, "defaults-no-bg");
}

TEST_F(NativeKeyerTest, EveryModeEveryShowWithBg)
{
    for (int m = 0; m < 4; ++m) {
        for (int s = 0; s < 4; ++s) {
            ParityPair pair = makePair(true, false);
            ASSERT_TRUE(bool(pair.native));
            setModeAndKey(pair, kModes[m], kGreenKey);
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamShow, std::string(kShows[s])));
            expectParity(pair, std::string("bg-") + kModes[m] + "-" + kShows[s], (m == 1) && (s == 3));
        }
    }
}

TEST_F(NativeKeyerTest, EveryModeEveryShowWithMasksAndNoBg)
{
    for (int m = 0; m < 4; ++m) {
        for (int s = 0; s < 4; ++s) {
            ParityPair pair = makePair(false, true);
            ASSERT_TRUE(bool(pair.native));
            setModeAndKey(pair, kModes[m], kGreenKey);
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamShow, std::string(kShows[s])));
            expectParity(pair, std::string("masks-") + kModes[m] + "-" + kShows[s], (m == 2) && (s == 1));
        }
    }
}

TEST_F(NativeKeyerTest, EveryLuminanceMath)
{
    const char* const maths[7] = { "rec709", "rec2020", "acesap0", "acesap1", "ccir601", "average", "max" };

    for (int m = 0; m < 7; ++m) {
        for (int mode = 0; mode < 2; ++mode) {
            ParityPair pair = makePair(true, false);
            ASSERT_TRUE(bool(pair.native));
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamLuminanceMath, std::string(maths[m])));
            // A black key colour makes Color mode fall back to the luminance too.
            const double black[3] = { 0., 0., 0. };
            setModeAndKey(pair, kModes[mode], mode ? black : kGreenKey);
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamCenter, { 0.5 }));
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamShow, std::string("composite")));
            expectParity(pair, std::string("luminance-math-") + maths[m] + (mode ? "-color" : "-luminance"));
        }
    }
}

TEST_F(NativeKeyerTest, ThresholdsAndTolerances)
{
    const double softnessLower[3] = { -0.3, 0., -1. };
    const double toleranceLower[3] = { -0.1, -0.2, 0. };
    const double center[3] = { 0.4, 0., 0.9 };
    const double toleranceUpper[3] = { 0.1, 0.3, 0.2 };
    const double softnessUpper[3] = { 0.2, 0., 0.5 };

    for (int k = 0; k < 3; ++k) {
        for (int mode = 0; mode < 3; ++mode) {
            ParityPair pair = makePair(true, false);
            ASSERT_TRUE(bool(pair.native));
            setModeAndKey(pair, kModes[mode], kGreenKey);
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamSoftnessLower, { softnessLower[k] }));
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamToleranceLower, { toleranceLower[k] }));
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamCenter, { center[k] }));
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamToleranceUpper, { toleranceUpper[k] }));
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamSoftnessUpper, { softnessUpper[k] }));
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamShow, std::string("premultiplied")));
            expectParity(pair, std::string("thresholds-") + std::to_string(k) + "-" + kModes[mode]);
        }
    }
}

TEST_F(NativeKeyerTest, DespillInScreenAndNone)
{
    const double despills[4] = { 0., 0.5, 1., 2. };
    const double angles[4] = { 120., 60., 180., 0. };
    const double screenKey[3] = { 0.1, 0.8, 0.2 };

    for (int mode = 2; mode < 4; ++mode) {
        for (int d = 0; d < 4; ++d) {
            ParityPair pair = makePair(true, true);
            ASSERT_TRUE(bool(pair.native));
            setModeAndKey(pair, kModes[mode], screenKey);
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamDespill, { despills[d] }));
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamDespillAngle, { angles[d] }));
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamShow, std::string("unpremultiplied")));
            expectParity(pair, std::string("despill-") + kModes[mode] + "-" + std::to_string(d), (mode == 3) && (d == 2));
        }
    }
}

TEST_F(NativeKeyerTest, SourceAlphaHandling)
{
    const char* const handling[3] = { "ignore", "inside", "normal" };

    for (int h = 0; h < 3; ++h) {
        for (int m = 0; m < 4; ++m) {
            ParityPair pair = makePair(true, h == 1);
            ASSERT_TRUE(bool(pair.native));
            setModeAndKey(pair, kModes[m], kGreenKey);
            ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamSourceAlpha, std::string(handling[h])));
            expectParity(pair, std::string("source-alpha-") + handling[h] + "-" + kModes[m]);
        }
    }
}

// The OpenFX plug-in reads the alpha of an RGB source out of bounds when it uses source alpha, so
// only the cases that never read it are compared.
TEST_F(NativeKeyerTest, RgbSource)
{
    for (int m = 0; m < 4; ++m) {
        ParityPair pair = makePair(true, true, "rgb");
        ASSERT_TRUE(bool(pair.native));
        setModeAndKey(pair, kModes[m], kGreenKey);
        ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamShow, std::string("composite")));
        expectParity(pair, std::string("rgb-source-") + kModes[m]);
    }
}

TEST_F(NativeKeyerTest, MaskInputsAreIgnoredWhileTheirSelectorIsOff)
{
    ParityPair pair = makePair(true, true);
    ASSERT_TRUE(bool(pair.native));
    setModeAndKey(pair, "color", kGreenKey);
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_InM", { 0. }));
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_OutM", { 0. }));
    expectParity(pair, "masks-disabled");
}

TEST_F(NativeKeyerTest, EditingTheKeyColorOrModeResetsTheThresholdsAsOfxDoes)
{
    ParityPair pair = makePair(false, false);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(bool(pair.ofx));
    const double key[3] = { 0.2, 0.6, 0.3 };

    for (int m = 0; m < 4; ++m) {
        ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamMode, std::string(kModes[m])));
        expectThresholdsMatch(pair, std::string("mode ") + kModes[m]);
        ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamKeyColor, { key[0], key[1], key[2] }));
        expectThresholdsMatch(pair, std::string("key colour in ") + kModes[m]);
    }

    ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamMode, std::string("luminance")));
    ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamLuminanceMath, std::string("rec2020")));
    ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamKeyColor, { 0.9, 0.1, 0.4 }));
    expectThresholdsMatch(pair, "rec2020 luminance");

    ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamMode, std::string("screen")));
    ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamKeyColor, { 0., 0., 0. }));
    expectThresholdsMatch(pair, "black screen key");
}

TEST_F(NativeKeyerTest, EditingTheKeyColorSetsTheLuminanceThresholds)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kKeyerID), kNativeKeyerMajor);
    ASSERT_TRUE(isNative(node));

    ASSERT_TRUE(setKnobValues(node, kKeyerParamKeyColor, { 0.5, 0.5, 0.5 }));
    EXPECT_NEAR(0.5, knobValue(node, kKeyerParamCenter), 1e-5);
    EXPECT_NEAR(-0.5, knobValue(node, kKeyerParamSoftnessLower), 1e-5);
    EXPECT_NEAR(0.5, knobValue(node, kKeyerParamSoftnessUpper), 1e-5);
    EXPECT_NEAR(0., knobValue(node, kKeyerParamToleranceLower), 1e-9);
    EXPECT_NEAR(0., knobValue(node, kKeyerParamToleranceUpper), 1e-9);
}

TEST_F(NativeKeyerTest, BothSchedulerModesAgreeWithNoUnplannedPull)
{
    ParityPair pair = makePair(true, true);
    ASSERT_TRUE(bool(pair.native));
    setModeAndKey(pair, "screen", kGreenKey);
    ASSERT_TRUE(setKnobValue(pair.native, kKeyerParamSourceAlpha, "normal"));
    ASSERT_TRUE(setKnobValue(pair.native, kKeyerParamShow, "composite"));

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
