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
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Project.h"
#include "Engine/PyParameter.h"

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
    const std::vector<std::string> excluded(1, "Z");
    master->setExcludedChannels(1, excluded);

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
    EXPECT_EQ(excluded, rows2[1].channels);

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

// A new channel select starts on a channel, so an explicit None has to be written out.
TEST_F(PyPlugExportTest, UserChannelSelectNoneRoundTripsThroughPyPlugExport)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    NodePtr groupNode = getApp()->createNode(groupArgs);
    ASSERT_TRUE(bool(groupNode));
    NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(groupNode->getEffectInstance());
    ASSERT_TRUE(bool(group));

    KnobChannelSelectPtr master = groupNode->getEffectInstance()->createChannelSelectKnob("userChannel", "User Channel");
    ASSERT_TRUE(bool(master));
    ASSERT_FALSE(master->isNone());
    master->setNone();
    ASSERT_TRUE(master->isNone());
    KnobPagePtr userPage = groupNode->getEffectInstance()->getOrCreateUserPageKnob();
    ASSERT_TRUE(bool(userPage));
    userPage->addKnob(master);

    QString output;
    group->exportGroupToPython(QString::fromUtf8("test.pyplug.channelselect"), QString::fromUtf8("ChannelSelectGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);
    EXPECT_TRUE(output.contains(QString::fromUtf8("param.setNone()")));

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

    KnobChannelSelectPtr master2 = std::dynamic_pointer_cast<KnobChannelSelect>(container->getKnobByName("userChannel"));
    ASSERT_TRUE(bool(master2));
    EXPECT_TRUE(master2->isNone());

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

// A Shuffle's mapping knob stores index-based rows (see KnobShuffleMap.h) that only mean
// something relative to the layers currently selected on its in1/in2/out1/out2 slots. The
// export must translate a row into a channel-name connect() call instead of the knob's own
// raw value, and must do so after in1/out1 have already been set so ShuffleMapParam::connect()
// resolves against the same layers the node had when the row was recorded. It must also emit
// only the channels that differ from the node kind's own default rows, which this test
// exercises on both a Shuffle (whose default mapping is empty) and a ShuffleCopy (whose
// default mapping wires out1.RGB <- in2.RGB explicitly).
TEST_F(PyPlugExportTest, ShuffleMappingRoundTripsThroughPyPlugExport)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> diffuseChannels;
    diffuseChannels.push_back("R");
    diffuseChannels.push_back("G");
    diffuseChannels.push_back("B");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("diffuse", "diffuse", "", diffuseChannels), LayerRegistryEntry::eOriginUser, &error)) << error;

    std::vector<std::string> spec2Channels;
    spec2Channels.push_back("R");
    spec2Channels.push_back("G");
    spec2Channels.push_back("B");
    spec2Channels.push_back("A");
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("spec2", "spec2", "", spec2Channels), LayerRegistryEntry::eOriginUser, &error)) << error;

    CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    NodePtr groupNode = getApp()->createNode(groupArgs);
    ASSERT_TRUE(bool(groupNode));
    NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(groupNode->getEffectInstance());
    ASSERT_TRUE(bool(group));
    NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group);
    ASSERT_TRUE(bool(collection));

    CreateNodeArgs shuffleArgs(PLUGINID_NATRON_SHUFFLE, collection);
    NodePtr shuffle = getApp()->createNode(shuffleArgs);
    ASSERT_TRUE(bool(shuffle)) << "node creation failed for " << PLUGINID_NATRON_SHUFFLE;
    const std::string shuffleScriptName = shuffle->getScriptName();

    KnobLayerSelectPtr in1 = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(kShuffleParamIn1));
    ASSERT_TRUE(bool(in1));
    in1->setLayer("diffuse");

    KnobLayerSelectPtr out1 = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(kShuffleParamOut1));
    ASSERT_TRUE(bool(out1));
    out1->setLayer("spec2");

    KnobShuffleMapPtr mapping = std::dynamic_pointer_cast<KnobShuffleMap>(shuffle->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(mapping));
    // out1.A <- 1 ("A" is channel index 3 of spec2's [R, G, B, A]).
    mapping->setSource(1, 3, ShuffleSource::makeOne());
    ASSERT_EQ(std::size_t(1), mapping->getRows().size());

    CreateNodeArgs shuffleCopyArgs(PLUGINID_NATRON_SHUFFLECOPY, collection);
    NodePtr shuffleCopy = getApp()->createNode(shuffleCopyArgs);
    ASSERT_TRUE(bool(shuffleCopy)) << "node creation failed for " << PLUGINID_NATRON_SHUFFLECOPY;
    const std::string shuffleCopyScriptName = shuffleCopy->getScriptName();

    KnobShuffleMapPtr mappingCopy = std::dynamic_pointer_cast<KnobShuffleMap>(shuffleCopy->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(mappingCopy));
    // ShuffleCopy's compiled default explicitly wires out1.R/G/B <- in2.R/G/B (in1 and in2
    // both default to Color). Removing out1.R's row falls back to its identity default,
    // in1.0 ("in1.R"), which differs from the compiled default, so the exporter must re-emit
    // it explicitly for the round trip to reproduce it.
    mappingCopy->setSource(1, 0, KnobShuffleMap::defaultSource(1, 0));
    ASSERT_EQ(std::size_t(2), mappingCopy->getRows().size());

    QString output;
    group->exportGroupToPython(QString::fromUtf8("test.pyplug.shufflemap"), QString::fromUtf8("ShuffleMapGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);

    EXPECT_TRUE(output.contains(QString::fromUtf8("app.addProjectLayer(\"diffuse\", [\"R\", \"G\", \"B\"])")));
    EXPECT_TRUE(output.contains(QString::fromUtf8("app.addProjectLayer(\"spec2\", [\"R\", \"G\", \"B\", \"A\"])")));
    EXPECT_TRUE(output.contains(QString::fromUtf8("param.connect(\"1\", \"out1.A\")")));
    EXPECT_TRUE(output.contains(QString::fromUtf8("param.connect(\"in1.R\", \"out1.R\")")));

    // out1.G and out1.B still match ShuffleCopy's compiled default (in2.G/in2.B), so they
    // must not be re-emitted.
    EXPECT_FALSE(output.contains(QString::fromUtf8("out1.G")));
    EXPECT_FALSE(output.contains(QString::fromUtf8("out1.B")));

    // The mapping knob's raw (index-based) value must not also be emitted alongside the
    // connect() calls: getParam("mapping") should be fetched exactly once per node.
    EXPECT_EQ(2, output.count(QString::fromUtf8("getParam(\"mapping\")")));
    // ShuffleCopy's bbox is left at its default, so the export does not mention it.
    EXPECT_FALSE(output.contains(QString::fromUtf8("getParam(\"" kShuffleCopyParamBBox "\")")));

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

    NodeCollectionPtr containerCollection = std::dynamic_pointer_cast<NodeCollection>(container->getEffectInstance());
    ASSERT_TRUE(bool(containerCollection));
    NodePtr shuffle2 = containerCollection->getNodeByName(shuffleScriptName);
    ASSERT_TRUE(bool(shuffle2));

    KnobLayerSelectPtr in1_2 = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle2->getKnobByName(kShuffleParamIn1));
    ASSERT_TRUE(bool(in1_2));
    EXPECT_EQ(std::string("diffuse"), in1_2->getLayer());

    KnobLayerSelectPtr out1_2 = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle2->getKnobByName(kShuffleParamOut1));
    ASSERT_TRUE(bool(out1_2));
    EXPECT_EQ(std::string("spec2"), out1_2->getLayer());

    KnobShuffleMapPtr mapping2 = std::dynamic_pointer_cast<KnobShuffleMap>(shuffle2->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(mapping2));
    std::vector<ShuffleMapRow> rows2 = mapping2->getRows();
    ASSERT_EQ(std::size_t(1), rows2.size());
    EXPECT_EQ(1, rows2[0].outSlot);
    EXPECT_EQ(3, rows2[0].outIndex);
    EXPECT_EQ(ShuffleSource::eOne, rows2[0].src.kind);

    NodePtr shuffleCopy2 = containerCollection->getNodeByName(shuffleCopyScriptName);
    ASSERT_TRUE(bool(shuffleCopy2));

    KnobShuffleMapPtr mappingCopy2 = std::dynamic_pointer_cast<KnobShuffleMap>(shuffleCopy2->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(mappingCopy2));
    // Replaying connect("in1.R", "out1.R") on a fresh node (whose default already has
    // out1.R <- in2.R) normalises the row away again, reproducing mappingCopy's original
    // two-row state exactly.
    EXPECT_EQ(std::size_t(2), mappingCopy2->getRows().size());
    ShuffleSource copyR2 = mappingCopy2->getSource(1, 0);
    EXPECT_EQ(ShuffleSource::eInput, copyR2.kind);
    EXPECT_EQ(1, copyR2.slot);
    EXPECT_EQ(0, copyR2.index);
    ShuffleSource copyG2 = mappingCopy2->getSource(1, 1);
    EXPECT_EQ(ShuffleSource::eInput, copyG2.kind);
    EXPECT_EQ(2, copyG2.slot);
    EXPECT_EQ(1, copyG2.index);
    ShuffleSource copyB2 = mappingCopy2->getSource(1, 2);
    EXPECT_EQ(ShuffleSource::eInput, copyB2.kind);
    EXPECT_EQ(2, copyB2.slot);
    EXPECT_EQ(2, copyB2.index);

    project->reset(false, true);
}

// A row can outlive the channel names it was recorded with: its slot set to None, or to a
// layer with fewer channels. Such a row is written by index ("in2.1"), which getSource()
// reports and connect() reads back, so the export neither drops it nor changes it.
TEST_F(PyPlugExportTest, ShuffleRowOnANoneOrNarrowerSlotRoundTripsByIndex)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> diffuseChannels;
    diffuseChannels.push_back("R");
    diffuseChannels.push_back("G");
    diffuseChannels.push_back("B");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("diffuse", "diffuse", "", diffuseChannels), LayerRegistryEntry::eOriginUser, &error)) << error;

    CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    NodePtr groupNode = getApp()->createNode(groupArgs);
    ASSERT_TRUE(bool(groupNode));
    NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(groupNode->getEffectInstance());
    ASSERT_TRUE(bool(group));
    NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group);
    ASSERT_TRUE(bool(collection));

    CreateNodeArgs shuffleArgs(PLUGINID_NATRON_SHUFFLE, collection);
    NodePtr shuffle = getApp()->createNode(shuffleArgs);
    ASSERT_TRUE(bool(shuffle)) << "node creation failed for " << PLUGINID_NATRON_SHUFFLE;
    const std::string shuffleScriptName = shuffle->getScriptName();

    KnobLayerSelectPtr in1 = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(kShuffleParamIn1));
    ASSERT_TRUE(bool(in1));
    in1->setLayer("diffuse");
    KnobLayerSelectPtr in2 = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(kShuffleParamIn2));
    ASSERT_TRUE(bool(in2));
    ASSERT_TRUE(in2->getLayer().empty());

    KnobShuffleMapPtr mapping = std::dynamic_pointer_cast<KnobShuffleMap>(shuffle->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(mapping));
    // out1.R <- in2's second channel, in2 being None; out1.G <- in1's fourth, diffuse having
    // three (index 3 isn't outIndex 1's default of in1.1, so the row stays explicit).
    mapping->setSource(1, 0, ShuffleSource::makeInput(2, 1));
    mapping->setSource(1, 1, ShuffleSource::makeInput(1, 3));
    ASSERT_EQ(std::size_t(2), mapping->getRows().size());

    ShuffleMapParam param(mapping);
    EXPECT_EQ(QString::fromUtf8("in2.1"), param.getSource(QString::fromUtf8("out1.R")));
    EXPECT_EQ(QString::fromUtf8("in1.3"), param.getSource(QString::fromUtf8("out1.G")));
    const std::map<std::string, std::string> connections = param.getConnections();
    ASSERT_EQ(std::size_t(1), connections.count("out1.R"));
    EXPECT_EQ(std::string("in2.1"), connections.find("out1.R")->second);

    QString output;
    group->exportGroupToPython(QString::fromUtf8("test.pyplug.shufflemapindex"), QString::fromUtf8("ShuffleMapIndexGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);

    EXPECT_TRUE(output.contains(QString::fromUtf8("param.connect(\"in2.1\", \"out1.R\")"))) << output.toStdString();
    EXPECT_TRUE(output.contains(QString::fromUtf8("param.connect(\"in1.3\", \"out1.G\")"))) << output.toStdString();

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

    NodeCollectionPtr containerCollection = std::dynamic_pointer_cast<NodeCollection>(container->getEffectInstance());
    ASSERT_TRUE(bool(containerCollection));
    NodePtr shuffle2 = containerCollection->getNodeByName(shuffleScriptName);
    ASSERT_TRUE(bool(shuffle2));

    KnobShuffleMapPtr mapping2 = std::dynamic_pointer_cast<KnobShuffleMap>(shuffle2->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(mapping2));
    EXPECT_EQ(std::size_t(2), mapping2->getRows().size());
    EXPECT_TRUE(ShuffleSource::makeInput(2, 1) == mapping2->getSource(1, 0));
    EXPECT_TRUE(ShuffleSource::makeInput(1, 3) == mapping2->getSource(1, 1));

    project->reset(false, true);
}
