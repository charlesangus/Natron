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

#include <ofxImageEffect.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Project.h"
#include "Engine/WriteNode.h"

NATRON_NAMESPACE_USING

namespace {

bool
isNativeWrite(const NodePtr& node)
{
    return node && dynamic_cast<NativeWrite*>(node->getEffectInstance().get());
}

bool
isWriteContainer(const NodePtr& node)
{
    return node && dynamic_cast<WriteNode*>(node->getEffectInstance().get());
}

} // namespace

class NativeWriteSkeletonTest
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

TEST_F(NativeWriteSkeletonTest, ExplicitMajorTwoIsTheNativeNode)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);

    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(isNativeWrite(node));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_WRITE, node->getMajorVersion());
    EXPECT_TRUE(node->getEffectInstance()->isWriter());
    EXPECT_TRUE(node->getEffectInstance()->isOutput());
    EXPECT_EQ(1, node->getEffectInstance()->getNInputs());
}

TEST_F(NativeWriteSkeletonTest, UnversionedWriteStillBuildsTheContainer)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE));

    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(isWriteContainer(node));
    EXPECT_FALSE(isNativeWrite(node));
}

TEST_F(NativeWriteSkeletonTest, OfxEncoderIDStillBuildsTheContainer)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_OFX_WRITEOIIO));

    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(isWriteContainer(node));
    EXPECT_FALSE(isNativeWrite(node));
}

TEST_F(NativeWriteSkeletonTest, HasRenderButtonAndFrameIncrement)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
    ASSERT_TRUE(isNativeWrite(node));

    KnobButton* render = dynamic_cast<KnobButton*>(node->getKnobByName(kNatronWriteParamStartRender).get());
    ASSERT_TRUE(render != NULL);
    EXPECT_TRUE(render->isRenderButton());

    KnobInt* incr = dynamic_cast<KnobInt*>(node->getKnobByName(kNatronWriteParamFrameStep).get());
    ASSERT_TRUE(incr != NULL);
    EXPECT_EQ(1, incr->getValue());
    EXPECT_EQ(1, node->getFrameStepKnobValue());
}

TEST_F(NativeWriteSkeletonTest, MajorAndFilenameSurviveSaveAndLoad)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
    ASSERT_TRUE(isNativeWrite(node));

    KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(std::string("/tmp/native-write-skeleton.png"));
    const std::string name = node->getScriptName();

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("native-write.ntp");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->getNodeByName(name).get() == NULL);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_TRUE(isNativeWrite(loaded));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_WRITE, loaded->getMajorVersion());
    KnobOutputFile* loadedFile = dynamic_cast<KnobOutputFile*>(loaded->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(loadedFile != NULL);
    EXPECT_EQ(std::string("/tmp/native-write-skeleton.png"), loadedFile->getValue());
}
