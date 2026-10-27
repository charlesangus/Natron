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

#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

class ClearAllCachesTest
    : public BaseTest {
};

// The entry is looked up under the node's own hash, the only one a query stores under.
TEST_F(ClearAllCachesTest, EmptiesTheActionsCacheOfANativeNode)
{
    getApp()->getProject()->reset(false, true);
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kNativeGeneratorParamFrameRange, { 3., 9. }));

    EffectInstancePtr effect = node->getEffectInstance();
    const U64 hash = node->getHashValue();
    ASSERT_FALSE(effect->hasCachedFrameRange(hash));

    double first = 0., last = 0.;
    effect->getFrameRange_public(hash, &first, &last);
    ASSERT_EQ(3., first);
    ASSERT_EQ(9., last);
    EXPECT_TRUE(effect->hasCachedFrameRange(hash)) << "the answer is cached under the node's hash";

    appPTR->clearAllCaches();

    EXPECT_FALSE(effect->hasCachedFrameRange(hash));
}
