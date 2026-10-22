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
#include <cctype>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Nodes/IO/OiioWriteSupport.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

bool
isNativeWrite(const NodePtr& node)
{
    return node && dynamic_cast<NativeWrite*>(node->getEffectInstance().get());
}

} // namespace

class NativeWriteEntryPointsTest
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

TEST_F(NativeWriteEntryPointsTest, UnversionedWriteIsTheNativeNode)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE));

    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(isNativeWrite(node));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_WRITE, node->getMajorVersion());
}

TEST_F(NativeWriteEntryPointsTest, ExplicitOfxWriterIDBuildsNothing)
{
    CreateNodeArgs args(PLUGINID_OFX_WRITEOIIO, getApp()->getProject());
    args.setProperty<bool>(kCreateNodeArgsPropSilent, true);

    EXPECT_FALSE(bool(getApp()->createNode(args)));
}

TEST_F(NativeWriteEntryPointsTest, CreateWriterBuildsANativeWriteForEveryFormat)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::vector<std::string>& writable = OiioWriteSupport::writableExtensions();
    ASSERT_FALSE(writable.empty());

    for (const std::string& ext : writable) {
        const std::string path = (tmp.path() + QString::fromUtf8("/a.") + QString::fromStdString(ext)).toStdString();
        CreateNodeArgs args(PLUGINID_NATRON_WRITE, getApp()->getProject());
        NodePtr node = getApp()->createWriter(path, args);
        ASSERT_TRUE(bool(node)) << ext;
        EXPECT_TRUE(isNativeWrite(node)) << ext;
        KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
        ASSERT_TRUE(file != NULL) << ext;
        EXPECT_EQ(path, file->getValue()) << ext;
        EXPECT_FALSE(node->hasPersistentMessage()) << ext;
    }
}

TEST_F(NativeWriteEntryPointsTest, CreateWriterWithAFrameRangeSelectsTheManualRange)
{
    CreateNodeArgs args(PLUGINID_NATRON_WRITE, getApp()->getProject());
    NodePtr node = getApp()->createWriter("/tmp/native-write-range.exr", args, 3, 9);

    ASSERT_TRUE(isNativeWrite(node));
    KnobChoice* range = dynamic_cast<KnobChoice*>(node->getKnobByName(kNatronWriteParamFrameRange).get());
    ASSERT_TRUE(range != NULL);
    EXPECT_EQ(std::string(kNatronWriteFrameRangeManual), range->getActiveEntry().id);
    KnobInt* first = dynamic_cast<KnobInt*>(node->getKnobByName(kNatronWriteParamFirstFrame).get());
    KnobInt* last = dynamic_cast<KnobInt*>(node->getKnobByName(kNatronWriteParamLastFrame).get());
    ASSERT_TRUE(first != NULL);
    ASSERT_TRUE(last != NULL);
    EXPECT_EQ(3, first->getValue());
    EXPECT_EQ(9, last->getValue());
}

TEST_F(NativeWriteEntryPointsTest, WriterLookupFollowsTheWritableExtensions)
{
    const std::vector<std::string>& writable = OiioWriteSupport::writableExtensions();
    ASSERT_FALSE(writable.empty());

    std::vector<std::string> supported;
    appPTR->getSupportedWriterFileFormats(&supported);
    EXPECT_EQ(writable, supported);

    for (const std::string& ext : writable) {
        EXPECT_EQ(std::string(PLUGINID_NATRON_WRITE), appPTR->getWriterPluginIDForFileType(ext)) << ext;
        std::string upper = ext;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return (char)std::toupper(ch); });
        EXPECT_EQ(std::string(PLUGINID_NATRON_WRITE), appPTR->getWriterPluginIDForFileType(upper)) << upper;
    }
    EXPECT_EQ(std::string(), appPTR->getWriterPluginIDForFileType("cr2"));
    EXPECT_EQ(std::string(), appPTR->getWriterPluginIDForFileType("mov"));
    EXPECT_EQ(std::string(), appPTR->getWriterPluginIDForFileType(""));
}

TEST_F(NativeWriteEntryPointsTest, UnversionedWriteKeepsItsClassThroughSaveAndLoad)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE));
    ASSERT_TRUE(isNativeWrite(node));
    const std::string name = node->getScriptName();

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("unversioned-write.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_TRUE(isNativeWrite(loaded));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_WRITE, loaded->getMajorVersion());
}
