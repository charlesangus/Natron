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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "SceneTestAuthoring.h"

#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/SceneLoadSet.h"

NATRON_NAMESPACE_USING

class SceneLoadSetCoverageTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    NodePtr reader(const CarCity& city)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));

        EXPECT_TRUE(bool(node));
        if (node) {
            KnobFilePtr file = std::dynamic_pointer_cast<KnobFile>(node->getKnobByName(kUSDReadParamFile));
            EXPECT_TRUE(bool(file));
            if (file) {
                file->setValue(city.rootPath());
            }
        }

        return node;
    }

    NodePtr prune(const NodePtr& input,
                  const std::string& pattern)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(kUSDPruneParamPrimPattern).get());
        EXPECT_TRUE(knob != NULL);
        if (knob) {
            knob->setValue(pattern);
        }
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }
};

TEST_F(SceneLoadSetCoverageTest, TheProbeCountsTheBuildingsOpenInTheProcess)
{
    CarCity city(2, 3);
    SceneQueryPool pool(2);
    const ScenePayload payload = city.payload();
    std::string error;

    EXPECT_EQ(6, city.getNumBuildings());
    EXPECT_EQ(0, city.openedBuildings());
    {
        SceneQueryPool::Lease lease = pool.borrow(payload, std::make_shared<const SceneLoadSet>(SceneLoadSet::none()), &error);
        ASSERT_TRUE(lease.isValid()) << error;
        EXPECT_EQ(0, city.openedBuildings());
    }
    pool.releaseIdle();
    {
        SceneQueryPool::Lease lease = pool.borrow(payload, &error);
        ASSERT_TRUE(lease.isValid()) << error;
        EXPECT_EQ(6, city.openedBuildings());
    }
    pool.releaseIdle();
}

TEST_F(SceneLoadSetCoverageTest, APruneEvaluatesTheSameOnALoadSetAsOnEverything)
{
    CarCity city(2, 2);
    NodePtr source = reader(city);
    NodePtr node = prune(source, "/World/Block_0//car*");
    ASSERT_TRUE(source && node);

    expectParity(node, loadingOnly({ "/World/Block_0" }));
}
