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

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/OfxEffectInstance.h"
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
    if (name == kUnPremultByKnobName) {
        return "unpremult";
    }
    if (name.rfind(kEnableMaskKnobName, 0) == 0) {
        return "maskEnable";
    }
    if (name.rfind(kMaskChannelKnobName, 0) == 0) {
        return "maskChannel";
    }
    if (name == "maskInvert") {
        return "maskInvert";
    }
    if (name == "mix") {
        return "mix";
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

// The recognised footer rows of the main page in page order, from the last separator on when
// the page has one.
std::vector<std::string>
footerRoles(const NodePtr& node)
{
    const KnobPagePtr page = mainPageOf(node);
    const KnobsVec children = page ? page->getChildren() : KnobsVec();
    std::vector<std::string> roles;

    for (std::size_t i = 0; i < children.size(); ++i) {
        const std::string role = roleOf(children[i]);
        if (role == "separator") {
            roles.clear();
        }
        if (!role.empty()) {
            roles.push_back(role);
        }
    }

    return roles;
}

} // namespace

class StageKnobFooterTest
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

TEST_F(StageKnobFooterTest, GradeFooterHasPremultMaskInvertMix)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE), PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(node.get() != nullptr);

    const std::vector<std::string> expected = { "separator", "unpremult", "maskEnable", "maskChannel", "maskInvert", "mix" };
    EXPECT_EQ(expected, footerRoles(node));
}

TEST_F(StageKnobFooterTest, BlurFooterHasMaskInvertMixWithoutPremult)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    ASSERT_TRUE(node.get() != nullptr);

    const std::vector<std::string> expected = { "separator", "maskEnable", "maskChannel", "maskInvert", "mix" };
    EXPECT_EQ(expected, footerRoles(node));
}

TEST_F(StageKnobFooterTest, OldBaseNodeKeepsItsOrder)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_INVERT), PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(node.get() != nullptr);

    const std::vector<std::string> roles = footerRoles(node);
    ASSERT_GE(roles.size(), 4u);
    const std::vector<std::string> tail(roles.end() - 4, roles.end());
    const std::vector<std::string> expected = { "maskEnable", "maskChannel", "maskInvert", "mix" };
    EXPECT_EQ(expected, tail);
}
