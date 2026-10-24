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
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RawTestSource.h"

#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawTemperature.h"
#include "Engine/Nodes/Raw/RawWbPresets.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 16;
const int kHeight = 12;

// Rows of Engine/Nodes/Raw/data/wb_presets.json, darktable's data/wb_presets.json.
const std::array<double, 3> k5DMarkIIIDaylight = { 2.007813, 1., 1.630859 };
const std::array<double, 3> k5DMarkIIIDaylightMinus9 = { 1.78418, 1., 1.907227 };
const std::array<double, 3> k5DMarkIIIDaylightPlus9 = { 2.275391, 1., 1.416016 };
const std::array<double, 3> kXPro2Daylight = { 1.94702, 1., 1.831126 };
// Canon PowerShot SX50 HS "daylight" gives tunings -9, 0 and 9 only.
const std::array<double, 3> kSX50Daylight0 = { 1.502585, 1., 1.871768 };
const std::array<double, 3> kSX50Daylight9 = { 1.868853, 1., 1.460041 };

float
rampValue(int x,
          int y,
          int /*c*/)
{
    return 0.05f + 0.01f * (float)x + 0.005f * (float)y;
}

ImageMetadata
cameraMetadata(const std::string& make,
               const std::string& model)
{
    ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);

    RawMetadata::setMake(&metadata, make);
    RawMetadata::setModel(&metadata, model);

    return metadata;
}
} // namespace

class NativeRawTemperaturePresetsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    static const RawWbPresets::Database& presets()
    {
        return RawWbPresets::database();
    }

    NodePtr createTemperature(const ImageMetadata& metadata)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWTEMPERATURE));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 1, rampValue, metadata);
        connectNodes(source, node, 0, true);

        return node;
    }

    static KnobChoicePtr knobChoice(const NodePtr& node,
                                    const char* name)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(name);
    }

    // Every photosite of the RGGB ramp mosaic is multiplied by the coefficient of its colour,
    // rounded to float as the node bakes it.
    void expectMultiplied(const NodePtr& node,
                          const std::array<double, 3>& coeffs)
    {
        const RawLayout layout = *RawLayout::fromPattern("RGGB");
        RenderedPlane plane;
        std::string error;

        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const float coeff = (float)coeffs[layout.colourAt(x, y)];
                ASSERT_EQ(rampValue(x, y, 0) * coeff, rawTestPlaneValue(plane, x, y, 0)) << x << "," << y;
            }
        }
    }
};

TEST_F(NativeRawTemperaturePresetsTest, TheDatabaseIsInstalledBesideTheExecutable)
{
    EXPECT_FALSE(RawWbPresets::presetsJsonPath().empty());
    EXPECT_FALSE(presets().empty());
}

TEST_F(NativeRawTemperaturePresetsTest, Canon5DMarkIIIDaylightIsTheDatabaseRow)
{
    EXPECT_EQ(k5DMarkIIIDaylight, RawWbPresets::presetMultipliers(presets(), "Canon", "EOS 5D Mark III", "daylight", 0));
    EXPECT_EQ(k5DMarkIIIDaylightMinus9, RawWbPresets::presetMultipliers(presets(), "Canon", "EOS 5D Mark III", "daylight", -9));
    EXPECT_EQ(k5DMarkIIIDaylightPlus9, RawWbPresets::presetMultipliers(presets(), "Canon", "EOS 5D Mark III", "daylight", 9));
    // A saved tuning beyond the setting's is clamped to it.
    EXPECT_EQ(k5DMarkIIIDaylightPlus9, RawWbPresets::presetMultipliers(presets(), "Canon", "EOS 5D Mark III", "daylight", 30));

    const std::vector<RawWbPresets::CameraPreset> camera = RawWbPresets::cameraPresets(presets(), "Canon", "EOS 5D Mark III");
    ASSERT_FALSE(camera.empty());
    EXPECT_EQ("daylight", camera[0].name);
    EXPECT_EQ(-9, presets()[camera[0].minTuningPos].tuning);
    EXPECT_EQ(0, presets()[camera[0].noTuningPos].tuning);
    EXPECT_EQ(9, presets()[camera[0].maxTuningPos].tuning);
}

TEST_F(NativeRawTemperaturePresetsTest, TheNodeAppliesTheChosenPreset)
{
    NodePtr node = createTemperature(cameraMetadata("Canon", "EOS 5D Mark III"));
    ASSERT_TRUE(bool(node));
    KnobChoicePtr preset = knobChoice(node, kRawTemperatureParamPreset);
    ASSERT_TRUE(bool(preset));
    ASSERT_LT(0, preset->getNumEntries());
    EXPECT_EQ("daylight", preset->getEntry(0).id);

    KnobIntPtr finetune = node->getEffectInstance()->getKnobByNameAndType<KnobInt>(kRawTemperatureParamFinetune);
    ASSERT_TRUE(bool(finetune));
    EXPECT_FALSE(finetune->getIsSecret());
    // Tuning the chosen setting chooses the Camera Preset mode.
    finetune->setValue(-9);
    EXPECT_EQ((int)eRawTemperatureModeCameraPreset, knobChoice(node, kRawTemperatureParamMode)->getValue());
    expectMultiplied(node, k5DMarkIIIDaylightMinus9);

    finetune->setValue(0);
    expectMultiplied(node, k5DMarkIIIDaylight);
}

TEST_F(NativeRawTemperaturePresetsTest, ATuningBetweenTwoIsInterpolatedAsDarktableDoes)
{
    // dt_wb_preset_interpolate(): t = (4 - 0) / (9 - 0), linear in the reciprocal multipliers.
    const double t = 4. / 9.;
    std::array<double, 3> expected;
    for (int k = 0; k < 3; ++k) {
        expected[k] = 1. / (((1. - t) / kSX50Daylight0[k]) + (t / kSX50Daylight9[k]));
    }
    const std::optional<std::array<double, 3>> interpolated = RawWbPresets::presetMultipliers(presets(), "Canon", "PowerShot SX50 HS", "daylight", 4);
    ASSERT_TRUE(interpolated.has_value());
    for (int k = 0; k < 3; ++k) {
        EXPECT_DOUBLE_EQ(expected[k], (*interpolated)[k]) << k;
    }
    EXPECT_EQ(kSX50Daylight9, RawWbPresets::presetMultipliers(presets(), "Canon", "PowerShot SX50 HS", "daylight", 9));
}

TEST_F(NativeRawTemperaturePresetsTest, TheFujifilmXPro2ListsItsSevenPresets)
{
    const std::vector<std::string> expected = { "daylight", "cloudy", "incandescent", "day white fluorescent",
                                                "daylight fluorescent", "white fluorescent", "underwater" };
    const std::vector<RawWbPresets::CameraPreset> camera = RawWbPresets::cameraPresets(presets(), "Fujifilm", "X-Pro2");
    ASSERT_EQ(expected.size(), camera.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i], camera[i].name) << i;
        // No tunings: one row each.
        EXPECT_EQ(camera[i].minTuningPos, camera[i].maxTuningPos) << i;
    }

    NodePtr node = createTemperature(cameraMetadata("Fujifilm", "X-Pro2"));
    ASSERT_TRUE(bool(node));
    const std::vector<ChoiceOption> entries = knobChoice(node, kRawTemperatureParamPreset)->getEntries_mt_safe();
    ASSERT_EQ(expected.size(), entries.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i], entries[i].id) << i;
    }
    EXPECT_TRUE(node->getEffectInstance()->getKnobByNameAndType<KnobInt>(kRawTemperatureParamFinetune)->getIsSecret());
}

TEST_F(NativeRawTemperaturePresetsTest, ACameraWithoutPresetsShowsNone)
{
    EXPECT_TRUE(RawWbPresets::cameraPresets(presets(), "Natron", "No Such Camera").empty());
    EXPECT_FALSE(RawWbPresets::presetMultipliers(presets(), "Natron", "No Such Camera", "daylight", 0).has_value());
    // The exact spelling, as darktable matches it.
    EXPECT_TRUE(RawWbPresets::cameraPresets(presets(), "canon", "EOS 5D Mark III").empty());

    NodePtr node = createTemperature(cameraMetadata("Natron", "No Such Camera"));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(0, knobChoice(node, kRawTemperatureParamPreset)->getNumEntries());
}

TEST_F(NativeRawTemperaturePresetsTest, WithoutAsShotOrMatrixAsShotTakesTheFirstPreset)
{
    // darktable's _find_coeffs(): the camera's first row, which for the X-Pro2 is daylight.
    NodePtr node = createTemperature(cameraMetadata("Fujifilm", "X-Pro2"));
    ASSERT_TRUE(bool(node));
    ASSERT_EQ((int)eRawTemperatureModeAsShot, knobChoice(node, kRawTemperatureParamMode)->getValue());
    expectMultiplied(node, kXPro2Daylight);

    // For the 5D Mark III the first row is daylight at tuning -9.
    NodePtr canon = createTemperature(cameraMetadata("Canon", "EOS 5D Mark III"));
    ASSERT_TRUE(bool(canon));
    expectMultiplied(canon, k5DMarkIIIDaylightMinus9);
}

TEST_F(NativeRawTemperaturePresetsTest, WithoutMatrixCameraReferenceTakesTheDaylightPreset)
{
    EXPECT_EQ(k5DMarkIIIDaylight, RawWbPresets::daylightPreset(presets(), "Canon", "EOS 5D Mark III"));

    NodePtr node = createTemperature(cameraMetadata("Canon", "EOS 5D Mark III"));
    ASSERT_TRUE(bool(node));
    knobChoice(node, kRawTemperatureParamMode)->setValue((int)eRawTemperatureModeCameraReference);
    expectMultiplied(node, k5DMarkIIIDaylight);
}

TEST_F(NativeRawTemperaturePresetsTest, AsShotMultipliersStillComeFirst)
{
    ImageMetadata metadata = cameraMetadata("Fujifilm", "X-Pro2");
    RawMetadata::setWbCoeffs(&metadata, { 2.4, 1.2, 1.8 });
    NodePtr node = createTemperature(metadata);
    ASSERT_TRUE(bool(node));
    expectMultiplied(node, { 2., 1., 1.5 });
}
