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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Color/ColorCorrect.h"
#include "Engine/Nodes/Color/Grade.h"

#include "Gui/ColorTrackballTarget.h"

NATRON_NAMESPACE_USING

namespace {

ColorKnobInfo
makeKnobInfo(const std::string& scriptName,
             bool enabled = true,
             bool visible = true)
{
    ColorKnobInfo info;

    info.scriptName = scriptName;
    info.enabled = enabled;
    info.visible = visible;

    return info;
}

// Grade's own seven colour knobs, in declaration order, standing in for a real Grade panel's
// orderedColorKnobs() result.
std::vector<ColorKnobInfo>
gradeKnobs()
{
    std::vector<ColorKnobInfo> knobs;

    knobs.push_back(makeKnobInfo(kGradeParamBlackPoint));
    knobs.push_back(makeKnobInfo(kGradeParamWhitePoint));
    knobs.push_back(makeKnobInfo(kGradeParamBlack));
    knobs.push_back(makeKnobInfo(kGradeParamWhite));
    knobs.push_back(makeKnobInfo(kGradeParamMultiply));
    knobs.push_back(makeKnobInfo(kGradeParamOffset));
    knobs.push_back(makeKnobInfo(kGradeParamGamma));

    return knobs;
}

std::vector<ColorKnobInfo>
colorCorrectKnobs()
{
    std::vector<ColorKnobInfo> knobs;
    const char* groups[] = { kColorCorrectGroupMaster, kColorCorrectGroupShadows, kColorCorrectGroupMidtones, kColorCorrectGroupHighlights };

    for (int g = 0; g < 4; ++g) {
        knobs.push_back(makeKnobInfo(std::string(groups[g]) + kColorCorrectParamSaturation));
        knobs.push_back(makeKnobInfo(std::string(groups[g]) + kColorCorrectParamContrast));
        knobs.push_back(makeKnobInfo(std::string(groups[g]) + kColorCorrectParamGamma));
        knobs.push_back(makeKnobInfo(std::string(groups[g]) + kColorCorrectParamGain));
        knobs.push_back(makeKnobInfo(std::string(groups[g]) + kColorCorrectParamOffset));
    }

    return knobs;
}

} // namespace

TEST(ColorTrackballTarget, GradeWithNothingTouchedPicksGain)
{
    std::vector<ColorKnobInfo> knobs = gradeKnobs();
    int idx = pickTargetIndex(PLUGINID_NATRON_GRADE, knobs, -1);

    ASSERT_GE(idx, 0);
    EXPECT_EQ(std::string(kGradeParamWhite), knobs[idx].scriptName);
}

TEST(ColorTrackballTarget, ColorCorrectWithNothingTouchedPicksMasterGain)
{
    std::vector<ColorKnobInfo> knobs = colorCorrectKnobs();
    int idx = pickTargetIndex(PLUGINID_NATRON_COLORCORRECT, knobs, -1);

    ASSERT_GE(idx, 0);
    EXPECT_EQ(std::string(kColorCorrectGroupMaster) + kColorCorrectParamGain, knobs[idx].scriptName);
}

TEST(ColorTrackballTarget, UnrecognisedPluginPicksFirstVisibleColorKnob)
{
    std::vector<ColorKnobInfo> knobs;

    knobs.push_back(makeKnobInfo("color"));
    int idx = pickTargetIndex("Constant", knobs, -1);

    ASSERT_GE(idx, 0);
    EXPECT_EQ(std::string("color"), knobs[idx].scriptName);
}

TEST(ColorTrackballTarget, NoColorKnobsPicksNone)
{
    std::vector<ColorKnobInfo> knobs;
    int idx = pickTargetIndex(PLUGINID_NATRON_GRADE, knobs, -1);

    EXPECT_EQ(-1, idx);
}

TEST(ColorTrackballTarget, TouchedKnobWins)
{
    std::vector<ColorKnobInfo> knobs = gradeKnobs();
    // "multiply" rather than the Grade fallback's "white", to tell the two apart.
    int touchedIndex = 4;

    ASSERT_EQ(std::string(kGradeParamMultiply), knobs[touchedIndex].scriptName);

    int idx = pickTargetIndex(PLUGINID_NATRON_GRADE, knobs, touchedIndex);

    EXPECT_EQ(touchedIndex, idx);
}

TEST(ColorTrackballTarget, TouchedSecretKnobFallsThroughToPluginFallback)
{
    std::vector<ColorKnobInfo> knobs = gradeKnobs();
    int touchedIndex = 4;

    knobs[touchedIndex].visible = false;

    int idx = pickTargetIndex(PLUGINID_NATRON_GRADE, knobs, touchedIndex);

    ASSERT_GE(idx, 0);
    EXPECT_EQ(std::string(kGradeParamWhite), knobs[idx].scriptName);
}

TEST(ColorTrackballTarget, TouchedDisabledKnobFallsThroughToPluginFallback)
{
    std::vector<ColorKnobInfo> knobs = gradeKnobs();
    int touchedIndex = 4;

    knobs[touchedIndex].enabled = false;

    int idx = pickTargetIndex(PLUGINID_NATRON_GRADE, knobs, touchedIndex);

    ASSERT_GE(idx, 0);
    EXPECT_EQ(std::string(kGradeParamWhite), knobs[idx].scriptName);
}

TEST(ColorTrackballTarget, OutOfRangeTouchedIndexIsIgnored)
{
    std::vector<ColorKnobInfo> knobs = gradeKnobs();
    int idx = pickTargetIndex(PLUGINID_NATRON_GRADE, knobs, (int)knobs.size());

    ASSERT_GE(idx, 0);
    EXPECT_EQ(std::string(kGradeParamWhite), knobs[idx].scriptName);
}

TEST(ColorTrackballTarget, SecretGainFallsThroughToFirstVisibleKnob)
{
    std::vector<ColorKnobInfo> knobs = gradeKnobs();

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        if (knobs[i].scriptName == kGradeParamWhite) {
            knobs[i].visible = false;
        }
    }

    int idx = pickTargetIndex(PLUGINID_NATRON_GRADE, knobs, -1);

    ASSERT_GE(idx, 0);
    EXPECT_EQ(std::string(kGradeParamBlackPoint), knobs[idx].scriptName);
}

TEST(ColorTrackballTarget, AllSecretKnobsPickNone)
{
    std::vector<ColorKnobInfo> knobs = gradeKnobs();

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        knobs[i].visible = false;
    }

    int idx = pickTargetIndex(PLUGINID_NATRON_GRADE, knobs, -1);

    EXPECT_EQ(-1, idx);
}

TEST(ColorTrackballTarget, DisabledGainFallsThroughToFirstEnabledKnob)
{
    std::vector<ColorKnobInfo> knobs = gradeKnobs();

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        if ((knobs[i].scriptName == kGradeParamWhite) || (knobs[i].scriptName == kGradeParamBlackPoint)) {
            knobs[i].enabled = false;
        }
    }

    int idx = pickTargetIndex(PLUGINID_NATRON_GRADE, knobs, -1);

    ASSERT_GE(idx, 0);
    EXPECT_TRUE(knobs[idx].enabled);
    EXPECT_NE(std::string(kGradeParamWhite), knobs[idx].scriptName);
    EXPECT_NE(std::string(kGradeParamBlackPoint), knobs[idx].scriptName);
}

TEST(ColorTrackballTarget, AllDisabledKnobsPickNone)
{
    std::vector<ColorKnobInfo> knobs = gradeKnobs();

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        knobs[i].enabled = false;
    }

    EXPECT_EQ(-1, pickTargetIndex(PLUGINID_NATRON_GRADE, knobs, -1));
}
