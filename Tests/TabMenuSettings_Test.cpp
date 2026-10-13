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
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

namespace {

// The mode names tabtabtab_nuke_core defines as MODE_*; the tab menu's adapter passes them through unchanged.
std::vector<std::string>
sortedModeNames()
{
    std::vector<std::string> modes;
    modes.push_back("anchored_fuzzy");
    modes.push_back("consecutive");
    modes.push_back("non_anchored_fuzzy");

    return modes;
}

std::vector<std::string>
splitOnComma(const std::string& text)
{
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, ',')) {
        parts.push_back(part);
    }

    return parts;
}

KnobChoicePtr
searchModesKnob()
{
    return appPTR->getCurrentSettings()->getKnobByNameAndType<KnobChoice>("tabMenuSearchModes");
}

KnobBoolPtr
scrollKnob()
{
    return appPTR->getCurrentSettings()->getKnobByNameAndType<KnobBool>("tabMenuScroll");
}

} // anon namespace

TEST_F(BaseTest, TabMenuSettingsDefaultToAnchoredFirstWithScrolling)
{
    KnobChoicePtr modes = searchModesKnob();
    KnobBoolPtr scroll = scrollKnob();

    ASSERT_TRUE(bool(modes));
    ASSERT_TRUE(bool(scroll));
    EXPECT_EQ(0, modes->getDefaultValue(0));
    std::vector<ChoiceOption> entries = modes->getEntries_mt_safe();
    ASSERT_FALSE(entries.empty());
    EXPECT_EQ(std::string("anchored_fuzzy,non_anchored_fuzzy,consecutive"), entries[0].id);
    EXPECT_TRUE(scroll->getDefaultValue(0));
}

TEST_F(BaseTest, TabMenuSettingsOfferEveryModeOrderOnce)
{
    KnobChoicePtr modes = searchModesKnob();

    ASSERT_TRUE(bool(modes));
    std::vector<ChoiceOption> entries = modes->getEntries_mt_safe();
    ASSERT_EQ(6u, entries.size());

    std::set<std::string> ids;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        ids.insert(entries[i].id);
        std::vector<std::string> order = splitOnComma(entries[i].id);
        std::sort(order.begin(), order.end());
        EXPECT_EQ(sortedModeNames(), order) << "option " << i << " is " << entries[i].id;
    }
    EXPECT_EQ(entries.size(), ids.size());
}

TEST_F(BaseTest, TabMenuSettingsScrollCanBeTurnedOff)
{
    KnobChoicePtr modes = searchModesKnob();
    KnobBoolPtr scroll = scrollKnob();

    ASSERT_TRUE(bool(modes));
    ASSERT_TRUE(bool(scroll));
    const int savedModes = modes->getValue();
    const bool savedScroll = scroll->getValue();

    scroll->setValue(false);
    EXPECT_FALSE(scroll->getValue());
    modes->setValue(5);
    EXPECT_EQ(std::string("consecutive,non_anchored_fuzzy,anchored_fuzzy"), modes->getActiveEntry().id);

    scroll->setValue(savedScroll);
    modes->setValue(savedModes);
    EXPECT_EQ(savedScroll, scroll->getValue());
    EXPECT_EQ(savedModes, modes->getValue());
}
