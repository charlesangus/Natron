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

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "ProjectBundleTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Project.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_USING

class MissingPluginLoadTest
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

TEST_F(MissingPluginLoadTest, PlaceholderKeepsItsNodeAndEdges)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr generator = createNode(_generatorPluginID);
    NodePtr missing = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr downstream = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(generator && missing && downstream);
    connectNodes(generator, missing, 0, true);
    connectNodes(missing, downstream, 0, true);

    const std::string generatorName = generator->getScriptName_mt_safe();
    const std::string missingName = missing->getScriptName_mt_safe();
    const std::string downstreamName = downstream->getScriptName_mt_safe();

    Serialization::ProjectSerialization saved;
    getApp()->getProject()->toSerialization(&saved);
    Serialization::NodeSerializationPtr grade = findSerializedNode(saved.nodes.nodes, PLUGINID_NATRON_GRADE);
    ASSERT_TRUE(bool(grade));
    grade->pluginID = "com.example.NotInstalled";

    getApp()->getProject()->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(getApp()->getProject(), tmp, QString::fromUtf8("missing-plugin.ntpz"), saved));

    NodePtr loadedGenerator = getApp()->getProject()->getNodeByName(generatorName);
    NodePtr placeholder = getApp()->getProject()->getNodeByName(missingName);
    NodePtr loadedDownstream = getApp()->getProject()->getNodeByName(downstreamName);
    ASSERT_TRUE(loadedGenerator && placeholder && loadedDownstream);

    EXPECT_EQ(std::string(PLUGINID_NATRON_MISSING_PLUGIN), placeholder->getPluginID());
    EXPECT_EQ(loadedGenerator, placeholder->getInput(0));
    EXPECT_EQ(placeholder, loadedDownstream->getInput(0));
}

TEST_F(MissingPluginLoadTest, HasMissingPluginNodeSeesOnlyPlaceholders)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr generator = createNode(_generatorPluginID);
    NodePtr missing = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(generator && missing);
    connectNodes(generator, missing, 0, true);
    EXPECT_FALSE(getApp()->getProject()->hasMissingPluginNode());

    Serialization::ProjectSerialization saved;
    getApp()->getProject()->toSerialization(&saved);
    Serialization::NodeSerializationPtr grade = findSerializedNode(saved.nodes.nodes, PLUGINID_NATRON_GRADE);
    ASSERT_TRUE(bool(grade));
    grade->pluginID = "com.example.NotInstalled";

    getApp()->getProject()->reset(false, true);
    ASSERT_TRUE(loadProjectBundle(getApp()->getProject(), tmp, QString::fromUtf8("missing-plugin.ntpz"), saved));
    EXPECT_TRUE(getApp()->getProject()->hasMissingPluginNode());
}
