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

#include <QString>

#include "BaseTest.h"
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorLookup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Color/Saturation.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

std::string
roleOf(const KnobIPtr& knob)
{
    const std::string name = knob->getName();

    if (dynamic_cast<KnobSeparator*>(knob.get())) {
        return "separator";
    }
    if ((name == "clampBlack") || (name == "clampWhite") || (name == kUnPremultByKnobName) || (name == "maskInvert") || (name == "mix")) {
        return name;
    }
    if (name.rfind(kEnableMaskKnobName, 0) == 0) {
        return "maskEnable";
    }
    if (name.rfind(kMaskChannelKnobName, 0) == 0) {
        return "maskChannel";
    }

    return std::string();
}

KnobPagePtr
mainPageOf(const NodePtr& node)
{
    const std::vector<KnobIPtr> knobs = node->getEffectInstance()->getKnobs_mt_safe();

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        KnobPagePtr page = std::dynamic_pointer_cast<KnobPage>(knobs[i]);
        if (!page) {
            continue;
        }
        const KnobsVec children = page->getChildren();
        for (std::size_t j = 0; j < children.size(); ++j) {
            if (children[j]->getName() == "mix") {
                return page;
            }
        }
    }

    return KnobPagePtr();
}

// Every visible knob of the main page from the separator that opens the footer on, by role. The
// footer opens at the last separator above the first non-separator footer knob, so separators the
// plug-in places higher on the page are not part of it; a knob that is not part of the footer shows
// up as its name, so a stray one fails the comparison.
std::vector<std::string>
footerTail(const NodePtr& node,
           bool* separatorAfterSeparator)
{
    const KnobPagePtr page = mainPageOf(node);
    const KnobsVec children = page ? page->getChildren() : KnobsVec();
    std::vector<KnobIPtr> visible;

    for (std::size_t i = 0; i < children.size(); ++i) {
        if (!children[i]->getIsSecret()) {
            visible.push_back(children[i]);
        }
    }

    *separatorAfterSeparator = false;
    for (std::size_t i = 1; i < visible.size(); ++i) {
        if ((roleOf(visible[i]) == "separator") && (roleOf(visible[i - 1]) == "separator")) {
            *separatorAfterSeparator = true;
        }
    }

    std::size_t firstFooterKnob = visible.size();
    for (std::size_t i = 0; i < visible.size(); ++i) {
        const std::string role = roleOf(visible[i]);
        if (!role.empty() && (role != "separator")) {
            firstFooterKnob = i;
            break;
        }
    }
    std::size_t start = firstFooterKnob;
    while ((start > 0) && (roleOf(visible[start - 1]) == "separator")) {
        --start;
    }

    std::vector<std::string> tail;
    for (std::size_t i = start; i < visible.size(); ++i) {
        const std::string role = roleOf(visible[i]);
        tail.push_back(role.empty() ? visible[i]->getName() : role);
    }

    return tail;
}

void
expectFooter(const NodePtr& node)
{
    ASSERT_TRUE(node.get() != nullptr);

    bool doubledSeparator = true;
    const std::vector<std::string> expected = {
        "separator", "clampBlack", "clampWhite", kUnPremultByKnobName, "maskEnable", "maskChannel", "maskInvert", "mix"
    };
    EXPECT_EQ(expected, footerTail(node, &doubledSeparator));
    EXPECT_FALSE(doubledSeparator);

    KnobIPtr clampBlack = node->getKnobByName("clampBlack");
    KnobIPtr clampWhite = node->getKnobByName("clampWhite");
    ASSERT_TRUE(clampBlack && clampWhite);
    EXPECT_FALSE(clampBlack->isNewLineActivated());
    EXPECT_TRUE(clampWhite->isNewLineActivated());
}

} // namespace

class KnobFooterOrderTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }
};

TEST_F(KnobFooterOrderTest, Grade)
{
    expectFooter(createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE), PLUGIN_MAJOR_NATRON_GRADE));
}

TEST_F(KnobFooterOrderTest, Saturation)
{
    expectFooter(createNode(QString::fromUtf8(PLUGINID_NATRON_SATURATION), PLUGIN_MAJOR_NATRON_SATURATION));
}

TEST_F(KnobFooterOrderTest, ColorLookup)
{
    expectFooter(createNode(QString::fromUtf8(PLUGINID_NATRON_COLORLOOKUP), PLUGIN_MAJOR_NATRON_COLORLOOKUP));
}

TEST_F(KnobFooterOrderTest, OfxColourPluginWithAPremultPair)
{
    expectFooter(createNode(QString::fromUtf8(kNatronTestPointMatrixPluginID), kNatronTestPointMatrixMajor));
}
