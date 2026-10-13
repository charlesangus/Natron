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
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

int
countPagesLabelled(const NodePtr& node, const std::string& label)
{
    const std::vector<KnobIPtr> knobs = node->getEffectInstance()->getKnobs_mt_safe();
    int count = 0;

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        if (dynamic_cast<KnobPage*>(knobs[i].get()) && knobs[i]->getLabel() == label) {
            ++count;
        }
    }

    return count;
}

} // namespace

class NativeIOInfoPageTest
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

TEST_F(NativeIOInfoPageTest, WriteHasExactlyOneInfoPage)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
    ASSERT_TRUE(node && dynamic_cast<NativeWrite*>(node->getEffectInstance().get()));
    EXPECT_EQ(1, countPagesLabelled(node, "Info"));
}

TEST_F(NativeIOInfoPageTest, ReadHasExactlyOneInfoPage)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
    ASSERT_TRUE(node && dynamic_cast<NativeRead*>(node->getEffectInstance().get()));
    EXPECT_EQ(1, countPagesLabelled(node, "Info"));
}
