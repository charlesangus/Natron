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

#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <OpenColorIO/OpenColorIO.h>

#include <ofxColour.h>
#include <ofxImageEffect.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Nodes/OCIO/OCIOColorSpace.h"
#include "Engine/Nodes/OCIO/OCIODisplay.h"
#include "Engine/Nodes/OCIO/OCIOLookTransform.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/Settings.h"

#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kStudioURI = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kCGURI = "ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kStudioOnlySpace = "Camera Rec.709";
const char* const kSharedSpace = "ACEScg";

const char* const kConfigFixtureID = kNatronTestOCIOConfigPluginID;
const char* const kOCIOColorSpaceID = PLUGINID_NATRON_OCIOCOLORSPACE;
const char* const kOCIODisplayID = PLUGINID_NATRON_OCIODISPLAY;
const char* const kOCIOLookTransformID = PLUGINID_NATRON_OCIOLOOKTRANSFORM;

const char* const kOCIOVar = "OCIO";
const char* const kPreferenceMarkerVar = "NATRON_OCIO_ENV_IS_PREFERENCE";

ProjectPtr
project()
{
    return appPTR->getTopLevelInstance()->getProject();
}

NodePtr
createNode(const std::string& pluginID)
{
    CreateNodeArgs args(pluginID, project());

    return appPTR->getTopLevelInstance()->createNode(args);
}

NodePtr
createNativeWrite()
{
    CreateNodeArgs args(PLUGINID_NATRON_WRITE, project());

    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, PLUGIN_MAJOR_NATRON_WRITE, 0);

    return appPTR->getTopLevelInstance()->createNode(args);
}

NodePtr
createReader(const std::string& fixture)
{
    CreateNodeArgs args(PLUGINID_NATRON_READ, project());

    args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);

    return appPTR->getTopLevelInstance()->createNode(args);
}

KnobStringBasePtr
stringKnob(const NodePtr& node,
           const char* name)
{
    return node ? std::dynamic_pointer_cast<KnobStringBase>(node->getKnobByName(name)) : KnobStringBasePtr();
}

KnobChoicePtr
choiceKnob(const NodePtr& node,
           const char* name)
{
    return node ? std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(name)) : KnobChoicePtr();
}

void
expectCarriesConfig(const NodePtr& node,
                    const std::string& source)
{
    ASSERT_TRUE(bool(node));
    KnobStringBasePtr knob = stringKnob(node, "ocioConfigFile");
    ASSERT_TRUE(bool(knob)) << node->getPluginID();
    EXPECT_EQ(source, knob->getValue()) << node->getPluginID();
    EXPECT_TRUE(knob->getIsSecret()) << node->getPluginID();
}

void
switchProjectConfig(const char* uri)
{
    KnobChoicePtr config = project()->getKnobByNameAndType<KnobChoice>("ocioConfig");

    ASSERT_TRUE(bool(config));
    config->setValueFromID(uri, 0);
    ASSERT_EQ(std::string(uri), project()->getColorManagement()->getConfigSource());
}

// Planted as a plug-in edit, which leaves the space unchosen by the user, as a loaded project's is.
void
plantSpace(const NodePtr& node,
           const char* knobName,
           const std::string& space)
{
    KnobStringBasePtr knob = stringKnob(node, knobName);

    ASSERT_TRUE(bool(knob));
    knob->setValue(space, ViewSpec::all(), 0, eValueChangedReasonPluginEdited, 0);
    ASSERT_EQ(space, knob->getValue());
}

void
plantInputSpace(const NodePtr& node,
                const std::string& space)
{
    plantSpace(node, kOCIOColorSpaceParamInputSpace, space);
}

// The menu labels are family paths; the ids are the colourspace names.
bool
hasOptionNamed(const std::vector<ChoiceOption>& options,
               const std::string& name)
{
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (options[i].id == name) {
            return true;
        }
    }

    return false;
}

bool
configDefines(const char* uri,
              const char* space)
{
    ProjectColorManagement cm;
    std::string error;

    if (cm.load(uri, std::string(), &error) != ProjectColorManagement::eLoadErrorNone) {
        return false;
    }

    return bool(cm.getConfig()->getColorSpace(space));
}

void
saveResetAndLoad(const QTemporaryDir& tmp,
                 const char* fileName)
{
    const QString dirPath = tmp.path() + QLatin1Char('/');
    QString saved;

    ASSERT_TRUE(project()->saveProject(dirPath, QString::fromUtf8(fileName), &saved));
    project()->reset(false, true);
    ASSERT_TRUE(project()->loadProject(dirPath, QString::fromUtf8(fileName)));
}

struct SavedEnvVar {
    bool wasSet;
    QByteArray value;
};

SavedEnvVar
saveEnvVar(const char* name)
{
    SavedEnvVar ret;

    ret.wasSet = qEnvironmentVariableIsSet(name);
    ret.value = qgetenv(name);

    return ret;
}

void
restoreEnvVar(const char* name,
              const SavedEnvVar& saved)
{
    if (saved.wasSet) {
        qputenv(name, saved.value);
    } else {
        qunsetenv(name);
    }
}

void
setWorkingSpace(const char* name)
{
    KnobChoicePtr workingSpace = project()->getKnobByNameAndType<KnobChoice>("workingSpace");

    ASSERT_TRUE(bool(workingSpace));
    workingSpace->setValueFromID(name, 0);
    ASSERT_EQ(std::string(name), project()->getWorkingColorSpace());
}

void
setOCIOOverride(const char* ocio)
{
    if (ocio) {
        qputenv(kOCIOVar, QByteArray(ocio));
    } else {
        qunsetenv(kOCIOVar);
    }
    qunsetenv(kPreferenceMarkerVar);
    Settings::recaptureOCIOEnvOverrideForTests();
}
} // namespace

class ProjectOCIOTest
    : public testing::Test {
protected:
    virtual void SetUp()
    {
        _savedOCIO = saveEnvVar(kOCIOVar);
        _savedMarker = saveEnvVar(kPreferenceMarkerVar);
        setOCIOOverride(0);
        project()->reset(false, true);
    }

    virtual void TearDown()
    {
        restoreEnvVar(kOCIOVar, _savedOCIO);
        restoreEnvVar(kPreferenceMarkerVar, _savedMarker);
        Settings::recaptureOCIOEnvOverrideForTests();
        project()->reset(false, true);
    }

private:
    SavedEnvVar _savedOCIO;
    SavedEnvVar _savedMarker;
};

TEST_F(ProjectOCIOTest, NewReadWriteAndColorSpaceNodesCarryTheProjectConfigHidden)
{
    NodePtr read = createReader("flat-rgb-only.exr");
    NodePtr write = createNativeWrite();
    NodePtr colorSpace = createNode(kConfigFixtureID);

    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(write));
    ASSERT_TRUE(bool(colorSpace));

    expectCarriesConfig(read, kStudioURI);
    expectCarriesConfig(write, kStudioURI);
    expectCarriesConfig(colorSpace, kStudioURI);
}

TEST_F(ProjectOCIOTest, SwitchingToCGUpdatesEveryNodeAndRebuildsTheColorSpaceMenu)
{
    ASSERT_TRUE(configDefines(kStudioURI, kStudioOnlySpace));
    ASSERT_FALSE(configDefines(kCGURI, kStudioOnlySpace));

    NodePtr read = createReader("flat-rgb-only.exr");
    NodePtr write = createNativeWrite();
    NodePtr colorSpace = createNode(kOCIOColorSpaceID);
    NodePtr configNode = createNode(kConfigFixtureID);
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(write));
    ASSERT_TRUE(bool(colorSpace));
    ASSERT_TRUE(bool(configNode));

    KnobChoicePtr inputMenu = choiceKnob(colorSpace, kOCIOColorSpaceParamInputSpace "Index");
    ASSERT_TRUE(bool(inputMenu));
    ASSERT_TRUE(hasOptionNamed(inputMenu->getEntries_mt_safe(), kStudioOnlySpace));

    switchProjectConfig(kCGURI);

    expectCarriesConfig(read, kCGURI);
    expectCarriesConfig(write, kCGURI);
    expectCarriesConfig(colorSpace, kCGURI);
    expectCarriesConfig(configNode, kCGURI);

    const std::vector<ChoiceOption> entries = inputMenu->getEntries_mt_safe();
    const std::vector<std::string> cgSpaces = project()->getColorManagement()->getColorSpaces();
    EXPECT_EQ(Project::colorSpaceOptions(*project()->getColorManagement()).size(), entries.size());
    EXPECT_FALSE(hasOptionNamed(entries, kStudioOnlySpace));
    for (std::size_t i = 0; i < cgSpaces.size(); ++i) {
        EXPECT_TRUE(hasOptionNamed(entries, cgSpaces[i])) << cgSpaces[i];
    }
}

TEST_F(ProjectOCIOTest, SwitchingToAConfigThatLacksAColorSpaceKeepsTheNameAndFlagsTheNode)
{
    ASSERT_TRUE(configDefines(kStudioURI, kStudioOnlySpace));
    ASSERT_FALSE(configDefines(kCGURI, kStudioOnlySpace));

    NodePtr colorSpace = createNode(kOCIOColorSpaceID);
    ASSERT_TRUE(bool(colorSpace));
    plantInputSpace(colorSpace, kStudioOnlySpace);
    EXPECT_FALSE(colorSpace->hasPersistentMessage());

    switchProjectConfig(kCGURI);

    EXPECT_EQ(std::string(kStudioOnlySpace), stringKnob(colorSpace, kOCIOColorSpaceParamInputSpace)->getValue());
    ASSERT_TRUE(colorSpace->hasPersistentMessage());
    QString message;
    int type = 0;
    colorSpace->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8(kStudioOnlySpace))) << message.toStdString();

    switchProjectConfig(kStudioURI);

    EXPECT_EQ(std::string(kStudioOnlySpace), stringKnob(colorSpace, kOCIOColorSpaceParamInputSpace)->getValue());
    EXPECT_FALSE(colorSpace->hasPersistentMessage());
}

TEST_F(ProjectOCIOTest, AColorSpaceTheConfigLacksIsAnErrorOnLoadThatClearsWhenTheConfigDefinesIt)
{
    ASSERT_TRUE(configDefines(kStudioURI, kStudioOnlySpace));
    ASSERT_FALSE(configDefines(kCGURI, kStudioOnlySpace));
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    switchProjectConfig(kCGURI);
    NodePtr colorSpace = createNode(kOCIOColorSpaceID);
    ASSERT_TRUE(bool(colorSpace));
    const std::string name = colorSpace->getScriptName_mt_safe();
    plantInputSpace(colorSpace, kStudioOnlySpace);
    EXPECT_TRUE(colorSpace->hasPersistentMessage());

    saveResetAndLoad(tmp, "cg-unresolved.ntpz");
    colorSpace = project()->getNodeByName(name);
    ASSERT_TRUE(bool(colorSpace));
    EXPECT_EQ(std::string(kStudioOnlySpace), stringKnob(colorSpace, kOCIOColorSpaceParamInputSpace)->getValue());
    EXPECT_TRUE(colorSpace->hasPersistentMessage());
    QString message;
    int type = 0;
    colorSpace->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8(kStudioOnlySpace))) << message.toStdString();

    switchProjectConfig(kStudioURI);

    EXPECT_EQ(std::string(kStudioOnlySpace), stringKnob(colorSpace, kOCIOColorSpaceParamInputSpace)->getValue());
    EXPECT_FALSE(colorSpace->hasPersistentMessage());
}

TEST_F(ProjectOCIOTest, TheCheckLeavesOtherPersistentMessagesAlone)
{
    NodePtr read = createReader("flat-rgb-only.exr");
    ASSERT_TRUE(bool(read));

    read->setPersistentMessage(eMessageTypeError, "some other failure");
    project()->reportUnresolvedOCIOColorSpaces();

    EXPECT_TRUE(read->hasPersistentMessage());
}

TEST_F(ProjectOCIOTest, ASavedAndReloadedProjectKeepsTheConfigTheCleanStateAndTheHiddenKnob)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    switchProjectConfig(kCGURI);
    NodePtr read = createReader("flat-rgb-only.exr");
    NodePtr write = createNativeWrite();
    NodePtr colorSpace = createNode(kOCIOColorSpaceID);
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(write));
    ASSERT_TRUE(bool(colorSpace));
    const std::string readName = read->getScriptName_mt_safe();
    const std::string writeName = write->getScriptName_mt_safe();
    const std::string colorSpaceName = colorSpace->getScriptName_mt_safe();

    saveResetAndLoad(tmp, "cg.ntpz");

    EXPECT_EQ(std::string(kCGURI), project()->getOCIOConfigSource());
    read = project()->getNodeByName(readName);
    write = project()->getNodeByName(writeName);
    colorSpace = project()->getNodeByName(colorSpaceName);
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(write));
    ASSERT_TRUE(bool(colorSpace));

    expectCarriesConfig(read, kCGURI);
    expectCarriesConfig(write, kCGURI);
    expectCarriesConfig(colorSpace, kCGURI);
    EXPECT_FALSE(colorSpace->hasPersistentMessage());
}

TEST_F(ProjectOCIOTest, UnderAnOCIOOverrideALoadedProjectsNodesUseTheOverrideAndAreCheckedAgainstIt)
{
    ASSERT_TRUE(configDefines(kStudioURI, kStudioOnlySpace));
    ASSERT_FALSE(configDefines(kCGURI, kStudioOnlySpace));
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');

    switchProjectConfig(kCGURI);
    NodePtr read = createReader("flat-rgb-only.exr");
    NodePtr colorSpace = createNode(kOCIOColorSpaceID);
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(colorSpace));
    const std::string readName = read->getScriptName_mt_safe();
    const std::string colorSpaceName = colorSpace->getScriptName_mt_safe();
    plantInputSpace(colorSpace, kStudioOnlySpace);
    QString saved;
    ASSERT_TRUE(project()->saveProject(dirPath, QString::fromUtf8("cg.ntpz"), &saved));

    setOCIOOverride(kStudioURI);
    project()->reset(false, true);
    ASSERT_TRUE(project()->loadProject(dirPath, QString::fromUtf8("cg.ntpz")));

    EXPECT_EQ(std::string(kStudioURI), project()->getOCIOConfigSource());
    read = project()->getNodeByName(readName);
    colorSpace = project()->getNodeByName(colorSpaceName);
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(colorSpace));
    expectCarriesConfig(read, kStudioURI);
    expectCarriesConfig(colorSpace, kStudioURI);
    EXPECT_EQ(std::string(kStudioOnlySpace), stringKnob(colorSpace, kOCIOColorSpaceParamInputSpace)->getValue());
    EXPECT_FALSE(colorSpace->hasPersistentMessage());

    setOCIOOverride(0);
    project()->reset(false, true);
    ASSERT_TRUE(project()->loadProject(dirPath, QString::fromUtf8("cg.ntpz")));

    colorSpace = project()->getNodeByName(colorSpaceName);
    ASSERT_TRUE(bool(colorSpace));
    expectCarriesConfig(colorSpace, kCGURI);
    EXPECT_TRUE(colorSpace->hasPersistentMessage());
}

TEST_F(ProjectOCIOTest, LookAndDisplayMenusFollowTheProjectConfig)
{
    NodePtr look = createNode(kOCIOLookTransformID);
    NodePtr display = createNode(kOCIODisplayID);
    ASSERT_TRUE(bool(look));
    ASSERT_TRUE(bool(display));
    KnobChoicePtr lookMenu = choiceKnob(look, kOCIOLookTransformParamLook);
    KnobChoicePtr displayMenu = choiceKnob(display, kOCIODisplayParamDisplay);
    ASSERT_TRUE(bool(lookMenu));
    ASSERT_TRUE(bool(displayMenu));

    const char* const uris[] = { kCGURI, kStudioURI };
    for (std::size_t u = 0; u < sizeof(uris) / sizeof(uris[0]); ++u) {
        switchProjectConfig(uris[u]);
        ProjectColorManagementPtr cm = project()->getColorManagement();

        expectCarriesConfig(look, uris[u]);
        expectCarriesConfig(display, uris[u]);

        EXPECT_EQ(cm->getLooks().size(), lookMenu->getEntries_mt_safe().size()) << uris[u];

        const std::vector<std::string> displays = cm->getDisplays();
        const std::vector<ChoiceOption> displayEntries = displayMenu->getEntries_mt_safe();
        ASSERT_EQ(displays.size(), displayEntries.size()) << uris[u];
        for (std::size_t i = 0; i < displays.size(); ++i) {
            EXPECT_EQ(displays[i], displayEntries[i].id) << uris[u];
        }
    }
}

TEST_F(ProjectOCIOTest, AHostOwnedConfigSwitchLeavesARoleNamedColorSpaceAsWritten)
{
    ASSERT_TRUE(configDefines(kCGURI, "scene_linear"));

    NodePtr colorSpace = createNode(kOCIOColorSpaceID);
    ASSERT_TRUE(bool(colorSpace));
    plantInputSpace(colorSpace, "scene_linear");
    plantSpace(colorSpace, kOCIOColorSpaceParamOutputSpace, "color_picking");

    switchProjectConfig(kCGURI);

    EXPECT_EQ(std::string("scene_linear"), stringKnob(colorSpace, kOCIOColorSpaceParamInputSpace)->getValue());
    EXPECT_EQ(std::string("color_picking"), stringKnob(colorSpace, kOCIOColorSpaceParamOutputSpace)->getValue());
    EXPECT_FALSE(colorSpace->hasPersistentMessage());
}

TEST_F(ProjectOCIOTest, ChangingTheWorkingSpaceMovesOnlyTheTechnicalSideOfExistingReadsAndWrites)
{
    NodePtr read = createReader("flat-rgb-only.exr");
    NodePtr write = createNativeWrite();
    NodePtr colorSpace = createNode(kOCIOColorSpaceID);
    ASSERT_TRUE(bool(write));
    ASSERT_TRUE(bool(colorSpace));
    write->setOutputFilesForWriter("/tmp/projectOcioWorkingSpace.exr");
    plantInputSpace(colorSpace, kSharedSpace);
    plantSpace(colorSpace, kOCIOColorSpaceParamOutputSpace, kSharedSpace);

    ASSERT_EQ(std::string(kSharedSpace), stringKnob(read, "ocioWorkingSpace")->getValue());
    ASSERT_EQ(std::string(kSharedSpace), stringKnob(write, "ocioWorkingSpace")->getValue());
    const std::string readFileSpace = stringKnob(read, "ocioInputSpace")->getValue();
    const std::string writeFileSpace = stringKnob(write, "ocioOutputSpace")->getValue();
    ASSERT_FALSE(writeFileSpace.empty());

    setWorkingSpace("ACEScct");

    EXPECT_EQ(std::string("ACEScct"), stringKnob(read, "ocioWorkingSpace")->getValue());
    EXPECT_EQ(std::string("ACEScct"), stringKnob(write, "ocioWorkingSpace")->getValue());
    EXPECT_EQ(readFileSpace, stringKnob(read, "ocioInputSpace")->getValue());
    EXPECT_EQ(writeFileSpace, stringKnob(write, "ocioOutputSpace")->getValue());
    EXPECT_EQ(std::string(kSharedSpace), stringKnob(colorSpace, kOCIOColorSpaceParamInputSpace)->getValue());
    EXPECT_EQ(std::string(kSharedSpace), stringKnob(colorSpace, kOCIOColorSpaceParamOutputSpace)->getValue());

    setWorkingSpace(kSharedSpace);

    EXPECT_EQ(std::string(kSharedSpace), stringKnob(read, "ocioWorkingSpace")->getValue());
    EXPECT_EQ(std::string(kSharedSpace), stringKnob(write, "ocioWorkingSpace")->getValue());
    EXPECT_EQ(readFileSpace, stringKnob(read, "ocioInputSpace")->getValue());
    EXPECT_EQ(writeFileSpace, stringKnob(write, "ocioOutputSpace")->getValue());
}

TEST_F(ProjectOCIOTest, AConfigFileThatFailsToLoadKeepsTheKnobButPublishesTheConfigInUse)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string missing = (tmp.path() + QString::fromUtf8("/missing.ocio")).toStdString();

    NodePtr read = createReader("flat-rgb-only.exr");

    KnobChoicePtr config = project()->getKnobByNameAndType<KnobChoice>("ocioConfig");
    KnobStringBasePtr configFile = project()->getKnobByNameAndType<KnobStringBase>("ocioConfigFile");
    ASSERT_TRUE(bool(config));
    ASSERT_TRUE(bool(configFile));
    config->setValueFromID("Custom config", 0);
    configFile->setValue(missing);

    EXPECT_EQ(missing, configFile->getValue());
    EXPECT_EQ(missing, project()->getRequestedOCIOConfigSource());
    EXPECT_EQ(std::string(kStudioURI), project()->getColorManagement()->getConfigSource());
    EXPECT_EQ(std::string(kStudioURI), project()->getOCIOConfigSource());
    EXPECT_FALSE(project()->getOCIOConfigError(0));
    expectCarriesConfig(read, kStudioURI);
}

namespace {
NodePtr
createConstantIntoNativeWriter(const std::string& path)
{
    NodePtr constant = createNode("net.sf.openfx.ConstantPlugin");
    NodePtr writer = createNativeWrite();
    if (writer) {
        writer->setOutputFilesForWriter(path);
    }
    EXPECT_TRUE(bool(constant));
    EXPECT_TRUE(bool(writer));
    if (!constant || !writer || !project()->connectNodes(0, constant, writer)) {
        return NodePtr();
    }

    return writer;
}

bool
renderFrameOne(const NodePtr& writer)
{
    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
    if (!writerEffect) {
        return false;
    }
    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
    appPTR->getTopLevelInstance()->startWritersRendering(eRenderModeBlocking, works);

    return true;
}
} // namespace

TEST_F(ProjectOCIOTest, AnOCIOOverrideThatFailsToLoadFailsRendersInsteadOfUsingAnotherConfig)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string missing = (tmp.path() + QString::fromUtf8("/missing.ocio")).toStdString();
    const std::string failedPath = (tmp.path() + QString::fromUtf8("/failed.png")).toStdString();
    const std::string renderedPath = (tmp.path() + QString::fromUtf8("/rendered.png")).toStdString();

    setOCIOOverride(missing.c_str());
    project()->reset(false, true);

    EXPECT_EQ(missing, project()->getRequestedOCIOConfigSource());
    const std::string inUse = project()->getColorManagement()->getConfigSource();
    ASSERT_FALSE(inUse.empty());
    EXPECT_NE(missing, inUse);
    EXPECT_EQ(inUse, project()->getOCIOConfigSource());
    std::string error;
    ASSERT_TRUE(project()->getOCIOConfigError(&error));
    EXPECT_NE(std::string::npos, error.find(missing)) << error;

    NodePtr writer = createConstantIntoNativeWriter(failedPath);
    ASSERT_TRUE(bool(writer));
    expectCarriesConfig(writer, inUse);
    ASSERT_TRUE(renderFrameOne(writer));
    EXPECT_FALSE(QFile::exists(QString::fromStdString(failedPath)));

    setOCIOOverride(0);
    project()->reset(false, true);
    EXPECT_FALSE(project()->getOCIOConfigError(0));

    writer = createConstantIntoNativeWriter(renderedPath);
    ASSERT_TRUE(bool(writer));
    ASSERT_TRUE(renderFrameOne(writer));
    EXPECT_TRUE(QFile::exists(QString::fromStdString(renderedPath)));
}
