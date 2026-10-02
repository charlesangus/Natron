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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>

#include <OpenColorIO/OpenColorIO.h>

#include <ofxImageEffect.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ReadNode.h"
#include "Engine/Settings.h"
#include "Engine/WriteNode.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kStudioURI = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kCGURI = "ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kStudioOnlySpace = "Camera Rec.709";
const char* const kSharedSpace = "ACEScg";

const char* const kOCIOColorSpaceID = "fr.inria.openfx.OCIOColorSpace";
const char* const kOCIODisplayID = "fr.inria.openfx.OCIODisplay";
const char* const kOCIOLookTransformID = "fr.inria.openfx.OCIOLookTransform";
const char* const kOCIOCDLTransformID = "fr.inria.openfx.OCIOCDLTransform";
const char* const kOCIOFileTransformID = "fr.inria.openfx.OCIOFileTransform";

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
createReader(const std::string& fixture)
{
    CreateNodeArgs args(PLUGINID_OFX_READOIIO, project());

    args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);

    return appPTR->getTopLevelInstance()->createNode(args);
}

NodePtr
embeddedNode(const NodePtr& node)
{
    EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

    if (ReadNode* isRead = dynamic_cast<ReadNode*>(effect.get())) {
        return isRead->getEmbeddedReader();
    }
    if (WriteNode* isWrite = dynamic_cast<WriteNode*>(effect.get())) {
        return isWrite->getEmbeddedWriter();
    }

    return NodePtr();
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

// Planted as a plug-in edit: a user edit would make the plug-in replace a name its config
// lacks before the host ever checks it.
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
    plantSpace(node, "ocioInputSpace", space);
}

bool
endsWith(const std::string& s,
         const std::string& suffix)
{
    return (s.size() >= suffix.size()) && (s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
}

bool
hasOptionNamed(const std::vector<ChoiceOption>& options,
               const std::string& name)
{
    for (std::size_t i = 0; i < options.size(); ++i) {
        if ((options[i].label == name) || endsWith(options[i].label, "/" + name)) {
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
    NodePtr write = createNode(PLUGINID_OFX_WRITEOIIO);
    NodePtr colorSpace = createNode(kOCIOColorSpaceID);

    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(write));
    ASSERT_TRUE(bool(colorSpace));
    ASSERT_TRUE(bool(embeddedNode(read)));
    ASSERT_TRUE(bool(embeddedNode(write)));

    expectCarriesConfig(read, kStudioURI);
    expectCarriesConfig(embeddedNode(read), kStudioURI);
    expectCarriesConfig(write, kStudioURI);
    expectCarriesConfig(embeddedNode(write), kStudioURI);
    expectCarriesConfig(colorSpace, kStudioURI);
}

TEST_F(ProjectOCIOTest, SwitchingToCGUpdatesEveryNodeAndRebuildsTheColorSpaceMenu)
{
    ASSERT_TRUE(configDefines(kStudioURI, kStudioOnlySpace));
    ASSERT_FALSE(configDefines(kCGURI, kStudioOnlySpace));

    NodePtr read = createReader("flat-rgb-only.exr");
    NodePtr write = createNode(PLUGINID_OFX_WRITEOIIO);
    NodePtr colorSpace = createNode(kOCIOColorSpaceID);
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(write));
    ASSERT_TRUE(bool(colorSpace));

    KnobChoicePtr inputMenu = choiceKnob(colorSpace, "ocioInputSpaceIndex");
    ASSERT_TRUE(bool(inputMenu));
    ASSERT_TRUE(hasOptionNamed(inputMenu->getEntries_mt_safe(), kStudioOnlySpace));

    switchProjectConfig(kCGURI);

    expectCarriesConfig(read, kCGURI);
    expectCarriesConfig(embeddedNode(read), kCGURI);
    expectCarriesConfig(write, kCGURI);
    expectCarriesConfig(embeddedNode(write), kCGURI);
    expectCarriesConfig(colorSpace, kCGURI);

    const std::vector<ChoiceOption> entries = inputMenu->getEntries_mt_safe();
    const std::vector<std::string> cgSpaces = project()->getColorManagement()->getColorSpaces();
    EXPECT_EQ(cgSpaces.size(), entries.size());
    EXPECT_FALSE(hasOptionNamed(entries, kStudioOnlySpace));
    for (std::size_t i = 0; i < cgSpaces.size(); ++i) {
        EXPECT_TRUE(hasOptionNamed(entries, cgSpaces[i])) << cgSpaces[i];
    }
}

TEST_F(ProjectOCIOTest, ANewDecoderAfterAFormatChangeCarriesTheProjectConfig)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr read = createReader("flat-rgb-only.exr");
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(embeddedNode(read)));
    const std::string exrDecoder = embeddedNode(read)->getPluginID();

    switchProjectConfig(kCGURI);

    KnobFilePtr file = std::dynamic_pointer_cast<KnobFile>(read->getKnobByName(kOfxImageEffectFileParamName));
    ASSERT_TRUE(bool(file));
    file->setValue((tmp.path() + QString::fromUtf8("/other.png")).toStdString());

    NodePtr decoder = embeddedNode(read);
    ASSERT_TRUE(bool(decoder));
    ASSERT_NE(exrDecoder, decoder->getPluginID());
    expectCarriesConfig(decoder, kCGURI);
    expectCarriesConfig(read, kCGURI);
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

    EXPECT_EQ(std::string(kStudioOnlySpace), stringKnob(colorSpace, "ocioInputSpace")->getValue());
    ASSERT_TRUE(colorSpace->hasPersistentMessage());
    QString message;
    int type = 0;
    colorSpace->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("ocioInputSpace = \"Camera Rec.709\" is not a colorspace in the "
                                                   "OpenColorIO config \"cg-config-v4.0.0_aces-v2.0_ocio-v2.5\".")))
        << message.toStdString();

    switchProjectConfig(kStudioURI);

    EXPECT_EQ(std::string(kStudioOnlySpace), stringKnob(colorSpace, "ocioInputSpace")->getValue());
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

    project()->reportUnresolvedOCIOColorSpaces();
    EXPECT_TRUE(colorSpace->hasPersistentMessage());

    saveResetAndLoad(tmp, "cg-unresolved.ntp");
    colorSpace = project()->getNodeByName(name);
    ASSERT_TRUE(bool(colorSpace));
    EXPECT_EQ(std::string(kStudioOnlySpace), stringKnob(colorSpace, "ocioInputSpace")->getValue());
    EXPECT_TRUE(colorSpace->hasPersistentMessage());
    QString message;
    int type = 0;
    colorSpace->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("ocioInputSpace = \"Camera Rec.709\" is not a colorspace in the "
                                                   "OpenColorIO config \"cg-config-v4.0.0_aces-v2.0_ocio-v2.5\".")))
        << message.toStdString();

    switchProjectConfig(kStudioURI);

    EXPECT_EQ(std::string(kStudioOnlySpace), stringKnob(colorSpace, "ocioInputSpace")->getValue());
    EXPECT_FALSE(colorSpace->hasPersistentMessage());
}

TEST_F(ProjectOCIOTest, TheCheckLeavesOtherPersistentMessagesAlone)
{
    NodePtr colorSpace = createNode(kOCIOColorSpaceID);
    ASSERT_TRUE(bool(colorSpace));

    colorSpace->setPersistentMessage(eMessageTypeError, "some other failure");
    project()->reportUnresolvedOCIOColorSpaces();

    EXPECT_TRUE(colorSpace->hasPersistentMessage());
}

TEST_F(ProjectOCIOTest, ASavedAndReloadedProjectKeepsTheConfigTheCleanStateAndTheHiddenKnob)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    switchProjectConfig(kCGURI);
    NodePtr read = createReader("flat-rgb-only.exr");
    NodePtr write = createNode(PLUGINID_OFX_WRITEOIIO);
    NodePtr colorSpace = createNode(kOCIOColorSpaceID);
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(write));
    ASSERT_TRUE(bool(colorSpace));
    const std::string readName = read->getScriptName_mt_safe();
    const std::string writeName = write->getScriptName_mt_safe();
    const std::string colorSpaceName = colorSpace->getScriptName_mt_safe();

    saveResetAndLoad(tmp, "cg.ntp");

    EXPECT_EQ(std::string(kCGURI), project()->getOCIOConfigSource());
    read = project()->getNodeByName(readName);
    write = project()->getNodeByName(writeName);
    colorSpace = project()->getNodeByName(colorSpaceName);
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(write));
    ASSERT_TRUE(bool(colorSpace));

    expectCarriesConfig(read, kCGURI);
    expectCarriesConfig(embeddedNode(read), kCGURI);
    expectCarriesConfig(write, kCGURI);
    expectCarriesConfig(embeddedNode(write), kCGURI);
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
    // The describe-time default is empty without an env config, and an empty destination
    // makes the plug-in raise its own persistent error on load.
    plantSpace(colorSpace, "ocioOutputSpace", kSharedSpace);
    QString saved;
    ASSERT_TRUE(project()->saveProject(dirPath, QString::fromUtf8("cg.ntp"), &saved));

    setOCIOOverride(kStudioURI);
    project()->reset(false, true);
    ASSERT_TRUE(project()->loadProject(dirPath, QString::fromUtf8("cg.ntp")));

    EXPECT_EQ(std::string(kStudioURI), project()->getOCIOConfigSource());
    read = project()->getNodeByName(readName);
    colorSpace = project()->getNodeByName(colorSpaceName);
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(bool(colorSpace));
    expectCarriesConfig(read, kStudioURI);
    expectCarriesConfig(embeddedNode(read), kStudioURI);
    expectCarriesConfig(colorSpace, kStudioURI);
    EXPECT_EQ(std::string(kStudioOnlySpace), stringKnob(colorSpace, "ocioInputSpace")->getValue());
    EXPECT_FALSE(colorSpace->hasPersistentMessage());

    setOCIOOverride(0);
    project()->reset(false, true);
    ASSERT_TRUE(project()->loadProject(dirPath, QString::fromUtf8("cg.ntp")));

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
    KnobChoicePtr lookMenu = choiceKnob(look, "lookChoice");
    KnobChoicePtr displayMenu = choiceKnob(display, "displayIndex");
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
            EXPECT_EQ(displays[i], displayEntries[i].label) << uris[u];
        }
    }
}

TEST_F(ProjectOCIOTest, CDLAndFileTransformsCarryThePushedConfig)
{
    NodePtr cdl = createNode(kOCIOCDLTransformID);
    NodePtr fileTransform = createNode(kOCIOFileTransformID);
    ASSERT_TRUE(bool(cdl));
    ASSERT_TRUE(bool(fileTransform));

    expectCarriesConfig(cdl, kStudioURI);
    expectCarriesConfig(fileTransform, kStudioURI);

    switchProjectConfig(kCGURI);

    expectCarriesConfig(cdl, kCGURI);
    expectCarriesConfig(fileTransform, kCGURI);
}
