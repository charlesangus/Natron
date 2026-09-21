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

CLANG_DIAG_OFF(deprecated)
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>
CLANG_DIAG_ON(deprecated)

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

// The export writes the layer channels using the exact case-sensitive Python list syntax
// produced by NodeCollection::exportGroupToPython(), so the test checks for this literal.
const char* const kExpectedMaskLine = "app.addProjectLayer(\"mask\", [\"A\"])";

} // namespace

class PyPlugExportTest
    : public BaseTest {
protected:
    // Builds a Group node containing a Roto node whose target layer is `layerID`, and
    // returns the group.
    NodeGroupPtr buildGroupWithRotoTargeting(const std::string& layerID)
    {
        CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
        NodePtr groupNode = getApp()->createNode(groupArgs);
        if (!groupNode) {
            return NodeGroupPtr();
        }
        NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(groupNode->getEffectInstance());
        if (!group) {
            return NodeGroupPtr();
        }
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group);

        CreateNodeArgs rotoArgs(PLUGINID_NATRON_ROTO, collection);
        NodePtr roto = getApp()->createNode(rotoArgs);
        if (!roto) {
            return NodeGroupPtr();
        }
        KnobLayerSelectPtr layer = std::dynamic_pointer_cast<KnobLayerSelect>(roto->getLayerKnob());
        if (!layer) {
            return NodeGroupPtr();
        }
        layer->setLayer(layerID);

        return group;
    }
};

// A Roto node targeting a project-registered, non-built-in layer must have that layer
// recreated by the exported script, and the recreation must happen before any node is
// created so that later addProjectLayer() lookups and createNode() calls both see it.
TEST_F(PyPlugExportTest, CustomLayerReferenceEmitsAddProjectLayerBeforeNodeCreation)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> channels(1, "A");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask", "mask", "", channels), LayerRegistryEntry::eOriginUser, &error)) << error;

    NodeGroupPtr group = buildGroupWithRotoTargeting("mask");
    ASSERT_TRUE(bool(group));

    QString output;
    group->exportGroupToPython(QString::fromUtf8("test.pyplug.mask"), QString::fromUtf8("MaskGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);

    QString maskLine = QString::fromUtf8(kExpectedMaskLine);
    EXPECT_EQ(1, output.count(maskLine));

    QString rotoCreation = QString::fromUtf8("\"") + QString::fromUtf8(PLUGINID_NATRON_ROTO) + QString::fromUtf8("\"");
    int maskLineIndex = output.indexOf(maskLine);
    int rotoCreationIndex = output.indexOf(rotoCreation);
    ASSERT_NE(-1, maskLineIndex);
    ASSERT_NE(-1, rotoCreationIndex);
    EXPECT_LT(maskLineIndex, rotoCreationIndex);

    project->reset(false, true);
}

// A Roto node left on its default Color target (a built-in layer) has nothing for the
// exported script to recreate.
TEST_F(PyPlugExportTest, BuiltInLayerReferenceEmitsNoAddProjectLayer)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodeGroupPtr group = buildGroupWithRotoTargeting(kNatronColorLayerID);
    ASSERT_TRUE(bool(group));

    QString output;
    group->exportGroupToPython(QString::fromUtf8("test.pyplug.color"), QString::fromUtf8("ColorGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);

    EXPECT_FALSE(output.contains(QString::fromUtf8("addProjectLayer")));

    project->reset(false, true);
}

// Round-trip: the exported script is handed to the embedded interpreter exactly like a
// loaded PyPlug is (see AppInstance::createNodeFromPythonModule(), which runs
// "<module>.createInstance(appN, appN.<container>)" against an already-created, empty
// container group). A full round-trip through AppManager's PyPlug file-discovery
// (loadPythonGroups()) is impractical here: that scan runs once at AppManager
// construction and the discovery entry point is private, so a test can't register a
// freshly-written .py file as a plugin without restarting the whole Tests binary. Running
// createInstance() against a real container node is the practical equivalent: it proves
// the addProjectLayer() call the export emitted actually re-populates the registry when
// executed by Python, ahead of the createNode() calls that depend on it.
TEST_F(PyPlugExportTest, ExportedAddProjectLayerCallRepopulatesRegistryOnLoad)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> channels(1, "A");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask", "mask", "", channels), LayerRegistryEntry::eOriginUser, &error)) << error;

    NodeGroupPtr group = buildGroupWithRotoTargeting("mask");
    ASSERT_TRUE(bool(group));

    QString output;
    group->exportGroupToPython(QString::fromUtf8("test.pyplug.mask"), QString::fromUtf8("MaskGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);
    ASSERT_TRUE(output.contains(QString::fromUtf8(kExpectedMaskLine)));

    // Also exercise the "write to a temp .py file" leg of the round-trip, even though the
    // file is read back in-process rather than discovered by AppManager.
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString scriptPath = tmpDir.path() + QString::fromUtf8("/MaskGroup.py");
    {
        QFile f(scriptPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream ts(&f);
        ts << output;
    }

    // Start over: a fresh project has neither the "mask" layer nor any nodes.
    project->reset(false, true);
    ImageLayerDesc unused;
    ASSERT_FALSE(project->findLayer("mask", &unused));

    QFile scriptFile(scriptPath);
    ASSERT_TRUE(scriptFile.open(QIODevice::ReadOnly | QIODevice::Text));
    std::string scriptText = QTextStream(&scriptFile).readAll().toStdString();

    std::string interpError, interpOutput;
    ASSERT_TRUE(interpretPythonScript(scriptText, &interpError, &interpOutput)) << interpError;

    CreateNodeArgs containerArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    containerArgs.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    NodePtr container = getApp()->createNode(containerArgs);
    ASSERT_TRUE(bool(container));

    std::string appVar = getApp()->getAppIDString();
    std::string callScript = "createInstance(" + appVar + ", " + appVar + "." + container->getFullyQualifiedName() + ")\n";
    ASSERT_TRUE(interpretPythonScript(callScript, &interpError, &interpOutput)) << interpError;

    ImageLayerDesc found;
    ASSERT_TRUE(project->findLayer("mask", &found));
    EXPECT_EQ(std::vector<std::string>(1, "A"), found.getChannels());

    project->reset(false, true);
}

// A user-created KnobChannelSet param aliased to an inner node's built-in "channels" knob
// (the EdgeBlur.py "Blur1channels" pattern) needs both its creation and its current rows in
// the exported script, since the generic alias-link exporter only wires an alias to a param
// that the script already created under the same name. The round trip below proves the
// reconstructed alias still forwards the master's rows to the aliased node's knob after reload.
TEST_F(PyPlugExportTest, UserChannelSetAliasRoundTripsThroughPyPlugExport)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    NodePtr groupNode = getApp()->createNode(groupArgs);
    ASSERT_TRUE(bool(groupNode));
    NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(groupNode->getEffectInstance());
    ASSERT_TRUE(bool(group));
    NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group);
    ASSERT_TRUE(bool(collection));

    CreateNodeArgs blurArgs(PLUGINID_OFX_BLURCIMG, collection);
    NodePtr blur = getApp()->createNode(blurArgs);
    ASSERT_TRUE(bool(blur));
    const std::string blurScriptName = blur->getScriptName();

    KnobChannelSetPtr blurChannels = std::dynamic_pointer_cast<KnobChannelSet>(blur->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(blurChannels));

    KnobChannelSetPtr master = groupNode->getEffectInstance()->createChannelSetKnob("Blur1channels", "Channels");
    ASSERT_TRUE(bool(master));
    KnobPagePtr userPage = groupNode->getEffectInstance()->getOrCreateUserPageKnob();
    ASSERT_TRUE(bool(userPage));
    userPage->addKnob(master);

    std::vector<std::string> maskChannels;
    maskChannels.push_back("R");
    maskChannels.push_back("G");
    master->setLayer(0, "mask", &maskChannels);
    master->addRegex("Z.*");

    ASSERT_TRUE(blurChannels->setKnobAsAliasOfThis(master, true));

    QString output;
    group->exportGroupToPython(QString::fromUtf8("test.pyplug.channelset"), QString::fromUtf8("ChannelSetGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);

    EXPECT_TRUE(output.contains(QString::fromUtf8("createChannelSetParam(\"Blur1channels\", \"Channels\")")));
    EXPECT_TRUE(output.contains(QString::fromUtf8(".setAsAlias(param)")));

    project->reset(false, true);

    std::string interpError, interpOutput;
    ASSERT_TRUE(interpretPythonScript(output.toStdString(), &interpError, &interpOutput)) << interpError;

    CreateNodeArgs containerArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    containerArgs.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    NodePtr container = getApp()->createNode(containerArgs);
    ASSERT_TRUE(bool(container));

    std::string appVar = getApp()->getAppIDString();
    std::string callScript = "createInstance(" + appVar + ", " + appVar + "." + container->getFullyQualifiedName() + ")\n";
    ASSERT_TRUE(interpretPythonScript(callScript, &interpError, &interpOutput)) << interpError;

    KnobChannelSetPtr master2 = std::dynamic_pointer_cast<KnobChannelSet>(container->getKnobByName("Blur1channels"));
    ASSERT_TRUE(bool(master2));

    std::vector<ChannelSetRow> rows2 = master2->getRows();
    ASSERT_EQ(std::size_t(2), rows2.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows2[0].mode);
    EXPECT_EQ(std::string("mask"), rows2[0].layerOrPattern);
    EXPECT_EQ(maskChannels, rows2[0].channels);
    EXPECT_EQ(ChannelSetRow::eModeRegex, rows2[1].mode);
    EXPECT_EQ(std::string("Z.*"), rows2[1].layerOrPattern);

    NodeCollectionPtr containerCollection = std::dynamic_pointer_cast<NodeCollection>(container->getEffectInstance());
    ASSERT_TRUE(bool(containerCollection));
    NodePtr blur2 = containerCollection->getNodeByName(blurScriptName);
    ASSERT_TRUE(bool(blur2));

    KnobChannelSetPtr blurChannels2 = std::dynamic_pointer_cast<KnobChannelSet>(blur2->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(blurChannels2));

    // The reconstructed alias forwards the rows the master was exported with...
    EXPECT_EQ(rows2, blurChannels2->getRows());

    // ...and keeps forwarding live changes after reload: setAll() only rewrites row 0
    // (the regex row is untouched), so the slave must mirror that same partial edit.
    master2->setAll();
    std::vector<ChannelSetRow> blurRowsAfterSetAll = blurChannels2->getRows();
    ASSERT_EQ(std::size_t(2), blurRowsAfterSetAll.size());
    EXPECT_EQ(ChannelSetRow::eModeAll, blurRowsAfterSetAll[0].mode);
    EXPECT_EQ(ChannelSetRow::eModeRegex, blurRowsAfterSetAll[1].mode);
    EXPECT_EQ(std::string("Z.*"), blurRowsAfterSetAll[1].layerOrPattern);

    project->reset(false, true);
}

// KnobLayerSelect::withChannelButtons is a constructor flag, not a persisted value: a
// user-created layer select saved and reloaded through ProjectSerialization must still report
// the flag it was created with (see Tests/ProjectSerialization_Test.cpp for the save/reset/load
// pattern this reuses).
TEST_F(PyPlugExportTest, UserLayerSelectWithChannelButtonsSurvivesProjectRoundTrip)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    NodePtr groupNode = getApp()->createNode(groupArgs);
    ASSERT_TRUE(bool(groupNode));

    KnobLayerSelectPtr master = groupNode->getEffectInstance()->createLayerSelectKnob("userLayer", "User Layer", true);
    ASSERT_TRUE(bool(master));
    ASSERT_TRUE(master->getWithChannelButtons());
    KnobPagePtr userPage = groupNode->getEffectInstance()->getOrCreateUserPageKnob();
    ASSERT_TRUE(bool(userPage));
    userPage->addKnob(master);

    const std::string groupName = groupNode->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QString::fromUtf8("/");
    const QString fileName = QString::fromUtf8("layerselect-roundtrip.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->getNodeByName(groupName).get() == NULL);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr groupNode2 = project->getNodeByName(groupName);
    ASSERT_TRUE(bool(groupNode2));

    KnobLayerSelectPtr master2 = std::dynamic_pointer_cast<KnobLayerSelect>(groupNode2->getKnobByName("userLayer"));
    ASSERT_TRUE(bool(master2));
    EXPECT_TRUE(master2->getWithChannelButtons());

    project->reset(false, true);
}
