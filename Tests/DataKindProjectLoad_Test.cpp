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

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "DataKindTestEffect.h"
#include "DeepRenderTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

// A root-level group with no Input or Output yet, and a node created inside it, so that each test
// wires its own chain.
NodePtr
createEmptyRootGroup(const ProjectPtr& project)
{
    CreateNodeArgs args(PLUGINID_NATRON_GROUP, project);

    args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

    return project->getApp()->createNode(args);
}

NodePtr
createInsideGroup(const NodePtr& group,
                  const std::string& pluginID)
{
    NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());

    if (!collection) {
        return NodePtr();
    }
    CreateNodeArgs args(pluginID, collection);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

    return group->getApp()->createNode(args);
}

NodePtr
innerNodeByName(const NodePtr& group,
                const std::string& name)
{
    NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());

    return collection ? collection->getNodeByName(name) : NodePtr();
}

bool
hasDataKindMessage(const NodePtr& node)
{
    if (!node->hasPersistentMessage()) {
        return false;
    }
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);

    return message.contains(QString::fromUtf8("kind of data"));
}

} // namespace

// Kinds are never serialized: a project can only end up on disk with a kind-invalid edge if it was
// wired up bypassing canConnectInput() (e.g. the way NodeWrapper::connectInput() does for Python
// scripts and PyPlug init). connectInput() itself hand-wires such an edge here, without going
// through canConnectInput(), to reproduce that. Loading it back must leave the user's graph exactly
// as they saved it and put the node holding the input it cannot handle into an error state -- the
// same answer as a project naming a colorspace its OpenColorIO config does not define.
TEST_F(BaseTest, ProjectLoadKeepsKindInvalidEdgeAndErrorsTheNode)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(deepSource && imageSink);
    ASSERT_TRUE(imageSink->connectInput(deepSource, 0));
    ASSERT_EQ(deepSource, imageSink->getInput(0));

    const std::string sinkName = imageSink->getScriptName();
    const std::string sourceName = deepSource->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("kind-invalid.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr sink2 = project->getNodeByName(sinkName);
    ASSERT_TRUE(bool(sink2));
    ASSERT_TRUE(bool(sink2->getInput(0)));
    EXPECT_EQ(sourceName, sink2->getInput(0)->getScriptName());
    EXPECT_TRUE(sink2->hasPersistentMessage());
}

// Every existing project is entirely image-kind: loading one must be a no-op for this check,
// edge and all, with no node left in an error state.
TEST_F(BaseTest, ProjectLoadKeepsValidEdgeAndLeavesNoErrorState)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr generator = createNode(_generatorPluginID);
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(generator && imageSink);
    connectNodes(generator, imageSink, 0, true);

    const std::string sinkName = imageSink->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("kind-valid.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr sink2 = project->getNodeByName(sinkName);
    ASSERT_TRUE(bool(sink2));
    EXPECT_TRUE(bool(sink2->getInput(0)));
    EXPECT_FALSE(sink2->hasPersistentMessage());
}

// Kinds are resolved from topology alone, so a reload -- which restores every edge in serialization
// order rather than the order the user made them -- has to arrive at the same answer. The Dot here
// is typed only by what it feeds, the direction with nothing upstream to fall back on.
TEST_F(BaseTest, ResolutionSurvivesSaveAndLoadUnchanged)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr deepSink = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSink));

    ASSERT_TRUE(dot && deepSink);
    connectNodes(dot, deepSink, 0, true);
    ASSERT_EQ(eDataKindDeep, dot->getEffectiveOutputDataKind());

    const std::string dotName = dot->getScriptName();
    const std::string sinkName = deepSink->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("kind-downstream.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr dot2 = project->getNodeByName(dotName);
    NodePtr sink2 = project->getNodeByName(sinkName);
    ASSERT_TRUE(dot2 && sink2);
    ASSERT_TRUE(bool(sink2->getInput(0)));

    bool ambiguous = true;
    EXPECT_EQ(eDataKindDeep, dot2->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_FALSE(ambiguous);
    EXPECT_FALSE(sink2->hasPersistentMessage());
    EXPECT_FALSE(dot2->hasPersistentMessage());
}

// A node the graph leaves ambiguous is not an invalid graph: both edges survive the round trip,
// nothing is rewired, and the node itself is not the one in error -- only a consumer that cannot
// take an ambiguous input would be.
TEST_F(BaseTest, ProjectLoadKeepsAmbiguousPolymorphicInputsIntact)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr generator = createNode(_generatorPluginID);
    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr poly = createNode(QString::fromUtf8(kTestPluginIDDataKindPolyTwoInputs));

    ASSERT_TRUE(generator && deepSource && poly);
    connectNodes(generator, poly, 0, true);
    connectNodes(deepSource, poly, 1, true);

    const std::string polyName = poly->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("kind-ambiguous.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr poly2 = project->getNodeByName(polyName);
    ASSERT_TRUE(bool(poly2));
    EXPECT_TRUE(bool(poly2->getInput(0)));
    EXPECT_TRUE(bool(poly2->getInput(1)));
    EXPECT_FALSE(poly2->hasPersistentMessage());

    bool ambiguous = false;
    EXPECT_EQ(eDataKindPolymorphic, poly2->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_TRUE(ambiguous);
}

// The other half of keeping an invalid edge instead of disconnecting it: once the user does what
// the error asked and removes the input the node cannot handle, the error has to go. An app that
// still says a project is broken after it has been fixed is no better than one that rewired it.
TEST_F(BaseTest, DataKindErrorClearsWhenTheOffendingInputIsRemoved)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(deepSource && imageSink);
    ASSERT_TRUE(imageSink->connectInput(deepSource, 0));

    const std::string sinkName = imageSink->getScriptName();
    const std::string sourceName = deepSource->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("kind-invalid-then-fixed.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr sink2 = project->getNodeByName(sinkName);
    NodePtr source2 = project->getNodeByName(sourceName);
    ASSERT_TRUE(sink2 && source2);
    ASSERT_TRUE(sink2->hasPersistentMessage());

    disconnectNodes(source2, sink2, true);

    EXPECT_FALSE(sink2->hasPersistentMessage());
}

// The persistent message is one slot per node with no record of who wrote it, so clearing the
// data-kind diagnostic must recognise that something else has written over it since and leave that
// alone: the node still has a real problem, just not this one.
TEST_F(BaseTest, ClearingTheDataKindErrorLeavesAnUnrelatedErrorAlone)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(deepSource && imageSink);
    ASSERT_TRUE(imageSink->connectInput(deepSource, 0));

    const std::string sinkName = imageSink->getScriptName();
    const std::string sourceName = deepSource->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("kind-invalid-plus-unrelated.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr sink2 = project->getNodeByName(sinkName);
    NodePtr source2 = project->getNodeByName(sourceName);
    ASSERT_TRUE(sink2 && source2);
    ASSERT_TRUE(sink2->hasPersistentMessage());

    const std::string unrelated("Something else went wrong on this node.");
    sink2->setPersistentMessage(eMessageTypeError, unrelated);

    disconnectNodes(source2, sink2, true);

    EXPECT_TRUE(sink2->hasPersistentMessage());

    QString message;
    int type = 0;
    sink2->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(unrelated, message.toStdString());
}

// An edge the engine only permits through an adapter must survive a round trip like any other:
// loading it back may neither drop it nor report it as a problem, since it is a legal graph. The
// edge is wired here with connectInput() rather than through canConnectInput() so that what this
// asserts is the whole-graph verdict taken at the end of the load, not the connection check
// (DataKind_Test.cpp's AdapterSinkAcceptsDeepSource covers that one).
TEST_F(BaseTest, AdapterEdgeSurvivesSaveAndLoadWithNoError)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr adapterSink = createNode(QString::fromUtf8(kTestPluginIDDataKindAdapterSink));

    ASSERT_TRUE(deepSource && adapterSink);
    ASSERT_TRUE(adapterSink->connectInput(deepSource, 0));
    ASSERT_EQ(deepSource, adapterSink->getInput(0));

    const std::string sinkName = adapterSink->getScriptName();
    const std::string sourceName = deepSource->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("kind-adapter.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr sink2 = project->getNodeByName(sinkName);
    ASSERT_TRUE(bool(sink2));
    ASSERT_TRUE(bool(sink2->getInput(0)));
    EXPECT_EQ(sourceName, sink2->getInput(0)->getScriptName());

    QString message;
    int type = 0;
    sink2->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty());
    EXPECT_FALSE(sink2->hasPersistentMessage());
}

// A deep chain inside a group is nothing a project file records: the group's kind follows what
// feeds its GroupOutput, so reloading has to re-derive deep at the group's output and at the
// GroupInput inside, and judge the edges on either side against that without finding a conflict.
TEST_F(BaseTest, DeepChainInsideGroupSurvivesSaveAndLoadWithSameKindsAndNoConflicts)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr group = createEmptyRootGroup(project);
    NodePtr deepSink = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSink));

    ASSERT_TRUE(deepSource && group && deepSink);
    NodePtr groupInput = createInsideGroup(group, PLUGINID_NATRON_INPUT);
    NodePtr innerGain = createInsideGroup(group, kTestPluginIDDeepRenderGain);
    NodePtr groupOutput = createInsideGroup(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(groupInput && innerGain && groupOutput);
    ASSERT_TRUE(innerGain->connectInput(groupInput, 0));
    ASSERT_TRUE(groupOutput->connectInput(innerGain, 0));
    ASSERT_EQ(Node::eCanConnectInput_ok, group->canConnectInput(deepSource, 0));
    ASSERT_TRUE(group->connectInput(deepSource, 0));
    ASSERT_EQ(Node::eCanConnectInput_ok, deepSink->canConnectInput(group, 0));
    ASSERT_TRUE(deepSink->connectInput(group, 0));

    bool ambiguous = true;
    ASSERT_EQ(eDataKindDeep, group->getEffectiveOutputDataKind(&ambiguous));
    ASSERT_FALSE(ambiguous);

    const std::string groupName = group->getScriptName();
    const std::string sinkName = deepSink->getScriptName();
    const std::string sourceName = deepSource->getScriptName();
    const std::string inputName = groupInput->getScriptName();
    const std::string gainName = innerGain->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("kind-group-deep.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr group2 = project->getNodeByName(groupName);
    NodePtr sink2 = project->getNodeByName(sinkName);
    ASSERT_TRUE(group2 && sink2);
    ASSERT_TRUE(bool(group2->getInput(0)));
    EXPECT_EQ(sourceName, group2->getInput(0)->getScriptName());
    ASSERT_TRUE(bool(sink2->getInput(0)));
    EXPECT_EQ(groupName, sink2->getInput(0)->getScriptName());

    ambiguous = true;
    EXPECT_EQ(eDataKindDeep, group2->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_FALSE(ambiguous);

    NodePtr input2 = innerNodeByName(group2, inputName);
    NodePtr gain2 = innerNodeByName(group2, gainName);
    ASSERT_TRUE(input2 && gain2);
    EXPECT_EQ(eDataKindDeep, input2->getEffectiveOutputDataKind());
    EXPECT_EQ(eDataKindDeep, gain2->getEffectiveOutputDataKind());

    NodesList all;
    project->getNodes_recursive(all, false);
    for (NodesList::const_iterator it = all.begin(); it != all.end(); ++it) {
        EXPECT_FALSE(hasDataKindMessage(*it)) << (*it)->getScriptName();
    }
}

// The same walk, with a chain inside the group that really cannot take what reaches the group
// from outside: the edge is kept as saved and the load reports it on the inner node that cannot
// handle it, the way a flat graph reports its own bad edge.
TEST_F(BaseTest, ConflictingChainInsideGroupReportsOnLoad)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr group = createEmptyRootGroup(project);

    ASSERT_TRUE(deepSource && group);
    NodePtr groupInput = createInsideGroup(group, PLUGINID_NATRON_INPUT);
    NodePtr innerImageSink = createInsideGroup(group, kTestPluginIDDataKindImageSink);
    NodePtr groupOutput = createInsideGroup(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(groupInput && innerImageSink && groupOutput);
    ASSERT_TRUE(innerImageSink->connectInput(groupInput, 0));
    ASSERT_TRUE(groupOutput->connectInput(innerImageSink, 0));

    // Wired past canConnectInput(), as a script or a PyPlug can.
    ASSERT_EQ(Node::eCanConnectInput_incompatibleDataKind, group->canConnectInput(deepSource, 0));
    ASSERT_TRUE(group->connectInput(deepSource, 0));

    const std::string groupName = group->getScriptName();
    const std::string sourceName = deepSource->getScriptName();
    const std::string sinkName = innerImageSink->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("kind-group-conflict.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr group2 = project->getNodeByName(groupName);
    ASSERT_TRUE(bool(group2));
    ASSERT_TRUE(bool(group2->getInput(0)));
    EXPECT_EQ(sourceName, group2->getInput(0)->getScriptName());

    NodePtr sink2 = innerNodeByName(group2, sinkName);
    ASSERT_TRUE(bool(sink2));
    EXPECT_TRUE(hasDataKindMessage(sink2));
}
