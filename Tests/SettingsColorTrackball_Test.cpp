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
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include <QObject>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

namespace {

struct ChangeCounter {
    int feedback = 0;
    int size = 0;
};

} // anon namespace

TEST_F(BaseTest, ColorTrackballSettingsDefaultToCrosshairRevealAndAThird)
{
    SettingsPtr settings = appPTR->getCurrentSettings();
    KnobChoicePtr feedback = settings->getKnobByNameAndType<KnobChoice>("colorTrackballFeedback");
    KnobChoicePtr size = settings->getKnobByNameAndType<KnobChoice>("colorTrackballWheelWidth");

    ASSERT_TRUE(bool(feedback));
    ASSERT_TRUE(bool(size));
    EXPECT_EQ(0, feedback->getDefaultValue(0));
    EXPECT_EQ((int)Settings::eColorTrackballWheelWidthThird, size->getDefaultValue(0));
    EXPECT_EQ(2u, feedback->getEntries_mt_safe().size());
    EXPECT_EQ(4u, size->getEntries_mt_safe().size());
    EXPECT_EQ(Settings::eColorTrackballFeedbackCrosshairReveal, settings->getColorTrackballFeedback());
    EXPECT_EQ(Settings::eColorTrackballWheelWidthThird, settings->getColorTrackballWheelWidth());
    EXPECT_DOUBLE_EQ(1. / 3., settings->getColorTrackballWheelFraction());
}

TEST_F(BaseTest, ColorTrackballWheelWidthMapsToItsFraction)
{
    SettingsPtr settings = appPTR->getCurrentSettings();
    KnobChoicePtr size = settings->getKnobByNameAndType<KnobChoice>("colorTrackballWheelWidth");
    ASSERT_TRUE(bool(size));
    const int saved = size->getValue();

    const double fractions[4] = { 1. / 2., 1. / 3., 1. / 4., 1. / 5. };
    for (int i = 0; i < 4; ++i) {
        size->setValue(i);
        EXPECT_DOUBLE_EQ(fractions[i], settings->getColorTrackballWheelFraction());
    }
    size->setValue(saved);
}

TEST_F(BaseTest, ColorTrackballSettingsEmitSettingChangedOncePerChange)
{
    SettingsPtr settings = appPTR->getCurrentSettings();
    KnobChoicePtr feedback = settings->getKnobByNameAndType<KnobChoice>("colorTrackballFeedback");
    KnobChoicePtr size = settings->getKnobByNameAndType<KnobChoice>("colorTrackballWheelWidth");
    ASSERT_TRUE(bool(feedback));
    ASSERT_TRUE(bool(size));

    ChangeCounter counter;
    const KnobI* feedbackKnob = feedback.get();
    const KnobI* sizeKnob = size.get();
    QMetaObject::Connection connection = QObject::connect(settings.get(), &Settings::settingChanged, settings.get(), [&](KnobI* knob) {
        if (knob == feedbackKnob) {
            ++counter.feedback;
        } else if (knob == sizeKnob) {
            ++counter.size;
        }
    });

    feedback->setValue(Settings::eColorTrackballFeedbackWidgetCursor);
    EXPECT_EQ(1, counter.feedback);
    EXPECT_EQ(0, counter.size);
    EXPECT_EQ(Settings::eColorTrackballFeedbackWidgetCursor, settings->getColorTrackballFeedback());

    size->setValue(Settings::eColorTrackballWheelWidthHalf);
    EXPECT_EQ(1, counter.feedback);
    EXPECT_EQ(1, counter.size);
    EXPECT_EQ(Settings::eColorTrackballWheelWidthHalf, settings->getColorTrackballWheelWidth());

    QObject::disconnect(connection);
    feedback->setValue(Settings::eColorTrackballFeedbackCrosshairReveal);
    size->setValue(Settings::eColorTrackballWheelWidthThird);
}

TEST_F(BaseTest, ColorTrackballSettingsRoundTripThroughSaveAndRestore)
{
    SettingsPtr settings = appPTR->getCurrentSettings();
    KnobChoicePtr feedback = settings->getKnobByNameAndType<KnobChoice>("colorTrackballFeedback");
    KnobChoicePtr size = settings->getKnobByNameAndType<KnobChoice>("colorTrackballWheelWidth");
    KnobBoolPtr saveEnabled = settings->getKnobByNameAndType<KnobBool>("saveSettings");
    ASSERT_TRUE(bool(feedback));
    ASSERT_TRUE(bool(size));
    ASSERT_TRUE(bool(saveEnabled));
    const bool savedEnabled = saveEnabled->getValue();
    saveEnabled->setValue(true);

    feedback->setValue(Settings::eColorTrackballFeedbackWidgetCursor);
    size->setValue(Settings::eColorTrackballWheelWidthFifth);
    std::vector<KnobI*> knobs;
    knobs.push_back(feedback.get());
    knobs.push_back(size.get());
    settings->saveSettings(knobs, false, false);

    feedback->setValue(Settings::eColorTrackballFeedbackCrosshairReveal);
    size->setValue(Settings::eColorTrackballWheelWidthThird);
    settings->restoreKnobsFromSettings(knobs);

    EXPECT_EQ(Settings::eColorTrackballFeedbackWidgetCursor, settings->getColorTrackballFeedback());
    EXPECT_EQ(Settings::eColorTrackballWheelWidthFifth, settings->getColorTrackballWheelWidth());

    feedback->setValue(Settings::eColorTrackballFeedbackCrosshairReveal);
    size->setValue(Settings::eColorTrackballWheelWidthThird);
    settings->saveSettings(knobs, false, false);
    saveEnabled->setValue(savedEnabled);
}
