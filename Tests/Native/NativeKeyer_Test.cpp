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
        EXPECT_FALSE(pair.live()) << "the OFX Keyer is retired, so parity replays the recorded references";
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
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Keyer " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
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

TEST_F(NativeKeyerTest, ColorModeWithBgComposite)
{
    ParityPair pair = makePair(true, false);
    ASSERT_TRUE(bool(pair.native));
    setModeAndKey(pair, "color", kGreenKey);
    ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamShow, std::string("composite")));
    expectParity(pair, "bg-color-composite", true);
}

TEST_F(NativeKeyerTest, ScreenModeWithMasksPremultiplied)
{
    ParityPair pair = makePair(false, true);
    ASSERT_TRUE(bool(pair.native));
    setModeAndKey(pair, "screen", kGreenKey);
    ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamShow, std::string("premultiplied")));
    expectParity(pair, "masks-screen-premultiplied", true);
}

TEST_F(NativeKeyerTest, DespillInNoneMode)
{
    ParityPair pair = makePair(true, true);
    ASSERT_TRUE(bool(pair.native));
    setModeAndKey(pair, "none", kGreenKey);
    ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamDespill, { 1. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamDespillAngle, { 180. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kKeyerParamShow, std::string("unpremultiplied")));
    expectParity(pair, "despill-none-2", true);
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
