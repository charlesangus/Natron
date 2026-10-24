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

#include <array>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RawTestSource.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawTemperature.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 24;
const int kHeight = 18;
// The spot covers the left half; the right half reads differently, so a pick that strays past
// the area gives other multipliers.
const int kSpotWidth = 12;

const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

// The left half's mean per colour, and what color_picker_apply() makes of it: green over each.
const std::array<float, 3> kSpotMean = { 0.5f, 0.25f, 0.4f };
const std::array<float, 3> kOutsideMean = { 0.1f, 0.6f, 0.2f };

// Each colour alternates between its mean plus and minus a step along x, so the mean over the
// left half, whose width is a multiple of every period, is the colour's mean.
RawTestSource::ValueFn
mosaicValue(const RawLayout& layout)
{
    return [layout](int x, int y, int) {
        const int c = (int)layout.colourAt(x, y);
        const float base = (x < kSpotWidth) ? kSpotMean[c] : kOutsideMean[c];
        const float step = ((x / 6 + y / 6) % 2 == 0) ? 0.05f : -0.05f;

        return base + step;
    };
}
} // namespace

class NativeRawTemperatureSpotTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createTemperature(int nComps,
                              const RawTestSource::ValueFn& value,
                              const ImageMetadata& metadata)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWTEMPERATURE));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), nComps, value, metadata);
        connectNodes(source, node, 0, true);

        return node;
    }

    static KnobDoublePtr knobDouble(const NodePtr& node,
                                    const char* name)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(name);
    }

    static KnobChoicePtr modeKnob(const NodePtr& node)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode);
    }

    // Sets the spot to the left half and presses Pick.
    static void pickLeftHalf(const NodePtr& node)
    {
        KnobDoublePtr bottomLeft = knobDouble(node, kRawTemperatureParamSpotBottomLeft);
        KnobDoublePtr size = knobDouble(node, kRawTemperatureParamSpotSize);
        KnobButtonPtr pick = node->getEffectInstance()->getKnobByNameAndType<KnobButton>(kRawTemperatureParamPick);
        ASSERT_TRUE(bottomLeft && size && pick);
        bottomLeft->setValue(0., ViewSpec::all(), 0);
        bottomLeft->setValue(0., ViewSpec::all(), 1);
        size->setValue((double)kSpotWidth, ViewSpec::all(), 0);
        size->setValue((double)kHeight, ViewSpec::all(), 1);
        pick->trigger();
    }

    static std::array<double, 3> coefficientKnobs(const NodePtr& node)
    {
        return { knobDouble(node, kRawTemperatureParamRed)->getValue(),
                 knobDouble(node, kRawTemperatureParamGreen)->getValue(),
                 knobDouble(node, kRawTemperatureParamBlue)->getValue() };
    }

    static RenderedPlane render(const NodePtr& node)
    {
        RenderedPlane plane;
        std::string error;

        EXPECT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;

        return plane;
    }

    // The picked multipliers are the expected ones, and rendering with them is rendering Custom
    // with the same knobs.
    void expectPicked(const NodePtr& node,
                      const std::array<float, 3>& mean)
    {
        const std::array<float, 3> expected = RawTemperatureOp::pickedCoefficients(mean);

        pickLeftHalf(node);
        EXPECT_EQ((int)eRawTemperatureModeFromImageArea, modeKnob(node)->getValue());
        const std::array<double, 3> coeffs = coefficientKnobs(node);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR((double)expected[c], coeffs[c], 1e-6) << c;
        }
        const RenderedPlane picked = render(node);
        modeKnob(node)->setValue((int)eRawTemperatureModeCustom);
        EXPECT_EQ(coeffs, coefficientKnobs(node));
        const RenderedPlane custom = render(node);
        ASSERT_EQ(picked.pixels.size(), custom.pixels.size());
        ASSERT_FALSE(picked.pixels.empty());
        for (std::size_t i = 0; i < picked.pixels.size(); ++i) {
            ASSERT_EQ(custom.pixels[i], picked.pixels[i]) << i;
        }
    }
};

TEST_F(NativeRawTemperatureSpotTest, TheCoefficientsAreColorPickerApplys)
{
    // Green over each mean: 0.25 / 0.5, 1, 0.25 / 0.4.
    const std::array<float, 3> coeffs = RawTemperatureOp::pickedCoefficients(kSpotMean);
    EXPECT_FLOAT_EQ(0.5f, coeffs[0]);
    EXPECT_EQ(1.f, coeffs[1]);
    EXPECT_FLOAT_EQ(0.625f, coeffs[2]);

    // Clamped to 8, and a mean at or under 0.001 counts as 1.
    const std::array<float, 3> clamped = RawTemperatureOp::pickedCoefficients({ 0.01f, 0.5f, 0.0005f });
    EXPECT_EQ(8.f, clamped[0]);
    EXPECT_FLOAT_EQ(0.5f, clamped[2]);
    const std::array<float, 3> noGreen = RawTemperatureOp::pickedCoefficients({ 0.5f, 0.f, 0.25f });
    EXPECT_FLOAT_EQ(2.f, noGreen[0]);
    EXPECT_EQ(1.f, noGreen[1]);
    EXPECT_FLOAT_EQ(4.f, noGreen[2]);
}

TEST_F(NativeRawTemperatureSpotTest, ABayerSpotIsAveragedPerColour)
{
    const RawLayout layout = *RawLayout::fromPattern("RGGB");
    NodePtr node = createTemperature(1, mosaicValue(layout), rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    expectPicked(node, kSpotMean);
}

TEST_F(NativeRawTemperatureSpotTest, AnXTransSpotIsAveragedPerColour)
{
    const RawLayout layout = *RawLayout::fromPattern(kXTransPattern);
    NodePtr node = createTemperature(1, mosaicValue(layout), rawTestMetadata(kXTransPattern, 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    expectPicked(node, kSpotMean);
}

TEST_F(NativeRawTemperatureSpotTest, ALinearSpotIsAveragedPerChannel)
{
    const RawTestSource::ValueFn value = [](int x, int y, int c) {
        const float base = (x < kSpotWidth) ? kSpotMean[c] : kOutsideMean[c];

        return base + (((x / 6 + y / 6) % 2 == 0) ? 0.05f : -0.05f);
    };
    NodePtr node = createTemperature(3, value, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    expectPicked(node, kSpotMean);
}

TEST_F(NativeRawTemperatureSpotTest, ChoosingTheModePicks)
{
    const RawLayout layout = *RawLayout::fromPattern("RGGB");
    NodePtr node = createTemperature(1, mosaicValue(layout), rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    KnobDoublePtr size = knobDouble(node, kRawTemperatureParamSpotSize);
    knobDouble(node, kRawTemperatureParamSpotBottomLeft)->setValue(0., ViewSpec::all(), 0);
    knobDouble(node, kRawTemperatureParamSpotBottomLeft)->setValue(0., ViewSpec::all(), 1);
    size->setValue((double)kSpotWidth, ViewSpec::all(), 0);
    size->setValue((double)kHeight, ViewSpec::all(), 1);

    modeKnob(node)->setValue((int)eRawTemperatureModeFromImageArea);
    const std::array<float, 3> expected = RawTemperatureOp::pickedCoefficients(kSpotMean);
    const std::array<double, 3> coeffs = coefficientKnobs(node);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR((double)expected[c], coeffs[c], 1e-6) << c;
    }
}

TEST_F(NativeRawTemperatureSpotTest, TheDefaultSpotIsDarktablesBoxOnTheSourceFormat)
{
    const RawLayout layout = *RawLayout::fromPattern("RGGB");
    NodePtr node = createTemperature(1, mosaicValue(layout), rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    const RectI format = node->getEffectInstance()->getInput(0)->getOutputFormat();
    ASSERT_FALSE(format.isNull());
    KnobDoublePtr bottomLeft = knobDouble(node, kRawTemperatureParamSpotBottomLeft);
    KnobDoublePtr size = knobDouble(node, kRawTemperatureParamSpotSize);
    EXPECT_DOUBLE_EQ(format.x1 + 0.02 * format.width(), bottomLeft->getValue(0));
    EXPECT_DOUBLE_EQ(format.y1 + 0.02 * format.height(), bottomLeft->getValue(1));
    EXPECT_DOUBLE_EQ(0.96 * format.width(), size->getValue(0));
    EXPECT_DOUBLE_EQ(0.96 * format.height(), size->getValue(1));
}
