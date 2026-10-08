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

#include <cstdlib>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

namespace {
bool
envOverrideActive()
{
    return AppManager::parseRenderSchedulerModeEnv(std::getenv("NATRON_RENDER_SCHEDULER")).has_value();
}

class RenderSchedulerModeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        _savedMode = appPTR->getRenderSchedulerMode();
        _savedKnob = knob()->getValue();
    }

    virtual void TearDown() OVERRIDE
    {
        knob()->setValue(_savedKnob);
        appPTR->setRenderSchedulerMode(_savedMode);
        BaseTest::TearDown();
    }

    static KnobChoicePtr knob()
    {
        return appPTR->getCurrentSettings()->getKnobByNameAndType<KnobChoice>("renderSchedulerMode");
    }

private:
    RenderSchedulerModeEnum _savedMode;
    int _savedKnob;
};
} // namespace

TEST(RenderSchedulerModeEnv, ParsesKnownValues)
{
    ASSERT_TRUE(AppManager::parseRenderSchedulerModeEnv("legacy").has_value());
    EXPECT_EQ(eRenderSchedulerModeLegacy, *AppManager::parseRenderSchedulerModeEnv("legacy"));
    ASSERT_TRUE(AppManager::parseRenderSchedulerModeEnv("taskgraph").has_value());
    EXPECT_EQ(eRenderSchedulerModeTaskGraph, *AppManager::parseRenderSchedulerModeEnv("taskgraph"));
}

TEST(RenderSchedulerModeEnv, RejectsEverythingElse)
{
    EXPECT_FALSE(AppManager::parseRenderSchedulerModeEnv("").has_value());
    EXPECT_FALSE(AppManager::parseRenderSchedulerModeEnv("garbage").has_value());
    EXPECT_FALSE(AppManager::parseRenderSchedulerModeEnv("TaskGraph").has_value());
    EXPECT_FALSE(AppManager::parseRenderSchedulerModeEnv(NULL).has_value());
}

TEST_F(RenderSchedulerModeTest, KnobExistsWithLegacyAndTaskGraphEntries)
{
    KnobChoicePtr k = knob();
    ASSERT_TRUE(k != NULL);
    EXPECT_EQ(2, (int)k->getNumEntries());
    EXPECT_EQ((int)eRenderSchedulerModeTaskGraph, k->getDefaultValue(0));
}

TEST_F(RenderSchedulerModeTest, KnobChangeIsReadBackUnlessEnvironmentOverrides)
{
    if (envOverrideActive()) {
        EXPECT_EQ(*AppManager::parseRenderSchedulerModeEnv(std::getenv("NATRON_RENDER_SCHEDULER")), appPTR->getRenderSchedulerMode());
        const RenderSchedulerModeEnum before = appPTR->getRenderSchedulerMode();
        knob()->setValue((int)eRenderSchedulerModeTaskGraph);
        knob()->setValue((int)eRenderSchedulerModeLegacy);
        EXPECT_EQ(before, appPTR->getRenderSchedulerMode());

        return;
    }
    knob()->setValue((int)eRenderSchedulerModeTaskGraph);
    EXPECT_EQ(eRenderSchedulerModeTaskGraph, appPTR->getCurrentSettings()->getRenderSchedulerMode());
    EXPECT_EQ(eRenderSchedulerModeTaskGraph, appPTR->getRenderSchedulerMode());
    knob()->setValue((int)eRenderSchedulerModeLegacy);
    EXPECT_EQ(eRenderSchedulerModeLegacy, appPTR->getRenderSchedulerMode());
}

TEST_F(RenderSchedulerModeTest, SetRenderSchedulerModeOverridesUntilTheNextSettingsChange)
{
    if (envOverrideActive()) {
        return;
    }
    knob()->setValue((int)eRenderSchedulerModeLegacy);
    appPTR->setRenderSchedulerMode(eRenderSchedulerModeTaskGraph);
    EXPECT_EQ(eRenderSchedulerModeTaskGraph, appPTR->getRenderSchedulerMode());
    EXPECT_EQ(eRenderSchedulerModeLegacy, appPTR->getCurrentSettings()->getRenderSchedulerMode());
    appPTR->setRenderSchedulerMode(eRenderSchedulerModeLegacy);
    EXPECT_EQ(eRenderSchedulerModeLegacy, appPTR->getRenderSchedulerMode());
}
