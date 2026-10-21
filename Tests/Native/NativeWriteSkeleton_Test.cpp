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

#include <initializer_list>
#include <string>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "NativePluginIdentity.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

bool
isNativeWrite(const NodePtr& node)
{
    return node && dynamic_cast<NativeWrite*>(node->getEffectInstance().get());
}

std::string
messageOf(const NodePtr& node)
{
    QString message;
    int type = 0;

    node->getPersistentMessage(&message, &type, false);

    return message.toStdString();
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

TEST_F(NativeWriteSkeletonTest, UnversionedWriteIsTheNativeNode)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE));

    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(isNativeWrite(node));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_WRITE, node->getMajorVersion());
}

TEST_F(NativeWriteSkeletonTest, OfxEncoderIDBuildsNothing)
{
    CreateNodeArgs args(PLUGINID_OFX_WRITEOIIO, getApp()->getProject());
    args.setProperty<bool>(kCreateNodeArgsPropSilent, true);

    EXPECT_FALSE(bool(getApp()->createNode(args)));
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

TEST_F(NativeWriteSkeletonTest, UnsupportedNamesPostTheMessageAtOnceAndASupportedNameClearsIt)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
    ASSERT_TRUE(isNativeWrite(node));
    KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(file != NULL);

    const char* const unsupported[] = { "/tmp/shot.cr2", "/tmp/shot.mov", "/tmp/shot.psd" };
    for (std::size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        file->setValue(std::string(unsupported[i]));
        EXPECT_TRUE(node->hasPersistentMessage()) << unsupported[i];
        EXPECT_NE(std::string::npos, messageOf(node).find("Unsupported format")) << messageOf(node);
        for (const char* name : { kNatronWriteParamBitDepth, kNatronWriteParamCompression, kNatronWriteParamQuality, kNatronWriteParamTileSize }) {
            KnobIPtr option = node->getKnobByName(name);
            ASSERT_TRUE(bool(option)) << name;
            EXPECT_TRUE(option->getIsSecret()) << name << " " << unsupported[i];
        }
    }

    file->setValue(std::string("/tmp/shot.png"));
    EXPECT_FALSE(node->hasPersistentMessage()) << messageOf(node);
}

TEST_F(NativeWriteSkeletonTest, UnsupportedNameInASavedProjectPostsTheMessageAndHidesTheOptionsOnLoad)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
    ASSERT_TRUE(isNativeWrite(node));
    KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(std::string("/tmp/native-write-skeleton.mov"));
    const std::string name = node->getScriptName();

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("native-write-mov.ntp");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    ASSERT_TRUE(isNativeWrite(loaded));
    EXPECT_TRUE(loaded->hasPersistentMessage());
    EXPECT_NE(std::string::npos, messageOf(loaded).find("Unsupported format")) << messageOf(loaded);
    for (const char* option : { kNatronWriteParamBitDepth, kNatronWriteParamCompression, kNatronWriteParamQuality, kNatronWriteParamTileSize }) {
        KnobIPtr knob = loaded->getKnobByName(option);
        ASSERT_TRUE(bool(knob)) << option;
        EXPECT_TRUE(knob->getIsSecret()) << option;
    }

    KnobOutputFile* loadedFile = dynamic_cast<KnobOutputFile*>(loaded->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(loadedFile != NULL);
    loadedFile->setValue(std::string("/tmp/native-write-skeleton.png"));
    EXPECT_FALSE(loaded->hasPersistentMessage()) << messageOf(loaded);
}

TEST_F(NativeWriteSkeletonTest, CreatingWithAFilenameInTheArgsKeepsItAndOpensNoDialog)
{
    CreateNodeArgs args(PLUGINID_NATRON_WRITE, getApp()->getProject());
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, PLUGIN_MAJOR_NATRON_WRITE, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, 0, 1);
    args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, "/tmp/native-write-args.png");

    NodePtr node = getApp()->createNode(args);
    ASSERT_TRUE(isNativeWrite(node));
    KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(file != NULL);
    EXPECT_EQ(std::string("/tmp/native-write-args.png"), file->getValue());
    EXPECT_TRUE(getApp()->isBackground());
}

TEST_F(NativeWriteSkeletonTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_WRITE, PLUGIN_MAJOR_NATRON_WRITE, "Write");
}
