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
#include <map>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <QtCore/QFile>
#include <QtCore/QObject>
#include <QtCore/QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_USING

namespace {

bool
contains(const NodesList& nodes,
         const NodePtr& node)
{
    return std::find(nodes.begin(), nodes.end(), node) != nodes.end();
}

} // namespace

class LinkedNodeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        original = createNode(_generatorPluginID);
        copy = createNode(_generatorPluginID);
        third = createNode(_generatorPluginID);
        ASSERT_TRUE(original && copy && third);

        const KnobsVec& knobs = original->getEffectInstance()->getKnobs();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            if ((*it)->isDeclaredByPlugin() && !(*it)->getIsSecret() && std::dynamic_pointer_cast<KnobDouble>(*it)) {
                knobName = (*it)->getName();
                break;
            }
        }
        ASSERT_FALSE(knobName.empty());
    }

    KnobDoublePtr knobOf(const NodePtr& node) const
    {
        return std::dynamic_pointer_cast<KnobDouble>(node->getEffectInstance()->getKnobByName(knobName));
    }

    NodePtr original, copy, third;
    std::string knobName;
};

TEST_F(LinkedNodeTest, EditsOnEitherNodeReachTheOther)
{
    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    EXPECT_TRUE(original->isLinkedNode());
    EXPECT_TRUE(copy->isLinkedNode());

    NodesList linked;
    original->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, copy));

    knobOf(copy)->setValue(0.25, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.25, knobOf(original)->getValue(0));

    knobOf(original)->setValue(0.75, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.75, knobOf(copy)->getValue(0));
}

TEST_F(LinkedNodeTest, ANodeLinkedToTheCopyStaysInSyncWithBoth)
{
    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    ASSERT_TRUE(third->linkAllKnobsTo(copy));

    knobOf(third)->setValue(0.5, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.5, knobOf(original)->getValue(0));
    EXPECT_DOUBLE_EQ(0.5, knobOf(copy)->getValue(0));

    knobOf(original)->setValue(0.125, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.125, knobOf(third)->getValue(0));

    NodesList linked;
    third->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, original));
    EXPECT_TRUE(contains(linked, copy));
}

TEST_F(LinkedNodeTest, UnlinkingOneNodeLeavesTheOtherTwoLinked)
{
    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    ASSERT_TRUE(third->linkAllKnobsTo(copy));
    knobOf(original)->setValue(0.5, ViewSpec::all(), 0);

    ASSERT_TRUE(copy->unlinkAllKnobs());
    EXPECT_FALSE(copy->isLinkedNode());
    EXPECT_DOUBLE_EQ(0.5, knobOf(copy)->getValue(0));

    NodesList linked;
    original->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, third));
    EXPECT_FALSE(contains(linked, copy));

    knobOf(third)->setValue(0.25, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.25, knobOf(original)->getValue(0));
    EXPECT_DOUBLE_EQ(0.5, knobOf(copy)->getValue(0));

    knobOf(copy)->setValue(1., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.25, knobOf(original)->getValue(0));
}

TEST_F(LinkedNodeTest, DeletingANodeUpdatesTheLinkedNodesOfTheRest)
{
    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    ASSERT_TRUE(third->linkAllKnobsTo(copy));

    int originalNotifications = 0;
    QObject::connect(original.get(), &Node::linksChanged, [&originalNotifications]() { ++originalNotifications; });

    third->destroyNode(true, false);
    EXPECT_GT(originalNotifications, 0);

    NodesList linked;
    original->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, copy));
    EXPECT_FALSE(contains(linked, third));

    knobOf(copy)->setValue(0.375, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.375, knobOf(original)->getValue(0));

    copy->destroyNode(true, false);
    EXPECT_FALSE(original->isLinkedNode());
}

TEST_F(LinkedNodeTest, LinkingAViewerIsRefused)
{
    // Background apps do not register the Viewer plugin, so the refusal is checked on the plugin ID.
    EXPECT_FALSE(Node::canPluginBeLinkedNode(PLUGINID_NATRON_VIEWER));
    EXPECT_TRUE(Node::canPluginBeLinkedNode(_generatorPluginID.toStdString()));

    CreateNodeArgs args(PLUGINID_NATRON_VIEWER, getApp()->getProject());
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    NodePtr viewer = getApp()->createNode(args);
    if (viewer) {
        EXPECT_FALSE(copy->linkAllKnobsTo(viewer));
        EXPECT_FALSE(viewer->isLinkedNode());
        EXPECT_FALSE(copy->isLinkedNode());
    }
}

TEST_F(LinkedNodeTest, APasteLinkedPartnerDoesNotBecomeALinkedNode)
{
    ASSERT_TRUE(knobOf(third)->linkTo(-1, knobOf(original), -1));
    ASSERT_TRUE(copy->linkAllKnobsTo(original));

    EXPECT_TRUE(original->isLinkedNode());
    EXPECT_TRUE(copy->isLinkedNode());
    EXPECT_FALSE(third->isLinkedNode());

    NodesList linked;
    original->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, copy));
    EXPECT_FALSE(contains(linked, third));

    // The paste link still holds: all three knobs share one group.
    knobOf(copy)->setValue(0.375, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.375, knobOf(third)->getValue(0));
    knobOf(third)->setValue(0.625, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.625, knobOf(copy)->getValue(0));
}

TEST_F(LinkedNodeTest, PasteLinkingOntoALinkedNodeDoesNotMakeALinkedNode)
{
    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    ASSERT_TRUE(knobOf(third)->linkTo(-1, knobOf(copy), -1));

    EXPECT_FALSE(third->isLinkedNode());
    NodesList linked;
    third->getLinkedNodes(&linked);
    EXPECT_TRUE(linked.empty());

    knobOf(original)->setValue(0.25, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.25, knobOf(third)->getValue(0));
}

TEST_F(LinkedNodeTest, LinkGroupsSurviveASaveAndReload)
{
    NodePtr pasteSource = createNode(_generatorPluginID);
    NodePtr pasteTarget = createNode(_generatorPluginID);
    ASSERT_TRUE(pasteSource && pasteTarget);

    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    ASSERT_TRUE(third->linkAllKnobsTo(original));
    knobOf(original)->setValue(0.25, ViewSpec::all(), 0);
    ASSERT_TRUE(knobOf(third)->unlinkFromGroups());
    knobOf(third)->setValue(0.875, ViewSpec::all(), 0);
    ASSERT_TRUE(knobOf(pasteTarget)->linkTo(-1, knobOf(pasteSource), -1));
    knobOf(pasteSource)->setValue(0.5, ViewSpec::all(), 0);

    const std::string originalName = original->getScriptName();
    const std::string copyName = copy->getScriptName();
    const std::string thirdName = third->getScriptName();
    const std::string pasteSourceName = pasteSource->getScriptName();
    const std::string pasteTargetName = pasteTarget->getScriptName();

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("linked-nodes.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    original.reset();
    copy.reset();
    third.reset();
    pasteSource.reset();
    pasteTarget.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    original = project->getNodeByName(originalName);
    copy = project->getNodeByName(copyName);
    third = project->getNodeByName(thirdName);
    pasteSource = project->getNodeByName(pasteSourceName);
    pasteTarget = project->getNodeByName(pasteTargetName);
    ASSERT_TRUE(original && copy && third && pasteSource && pasteTarget);

    // The three Linked Nodes are linked again, two-way.
    EXPECT_TRUE(original->isLinkedNode());
    EXPECT_TRUE(copy->isLinkedNode());
    EXPECT_TRUE(third->isLinkedNode());
    NodesList linked;
    original->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, copy));
    EXPECT_TRUE(contains(linked, third));
    EXPECT_DOUBLE_EQ(0.25, knobOf(copy)->getValue(0));
    knobOf(copy)->setValue(0.375, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.375, knobOf(original)->getValue(0));
    knobOf(original)->setValue(0.625, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.625, knobOf(copy)->getValue(0));

    // The unlinked knob kept its own value and re-links on request.
    EXPECT_FALSE(knobOf(third)->isLinked(0));
    EXPECT_TRUE(knobOf(third)->hasRelinkableGroups());
    EXPECT_DOUBLE_EQ(0.875, knobOf(third)->getValue(0));
    ASSERT_TRUE(knobOf(third)->relinkToGroups());
    EXPECT_DOUBLE_EQ(0.625, knobOf(third)->getValue(0));
    knobOf(third)->setValue(0.125, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.125, knobOf(original)->getValue(0));

    // The Paste Link survives, and does not make Linked Nodes.
    EXPECT_TRUE(knobOf(pasteTarget)->isLinked(0));
    EXPECT_FALSE(pasteSource->isLinkedNode());
    EXPECT_FALSE(pasteTarget->isLinkedNode());
    EXPECT_DOUBLE_EQ(0.5, knobOf(pasteTarget)->getValue(0));
    knobOf(pasteTarget)->setValue(0.75, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.75, knobOf(pasteSource)->getValue(0));
    EXPECT_DOUBLE_EQ(0.125, knobOf(original)->getValue(0));
}

TEST_F(LinkedNodeTest, PastedCopiesOfLinkedNodesAreLinkedOnlyToEachOther)
{
    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    knobOf(original)->setValue(0.25, ViewSpec::all(), 0);

    // What a paste does: the copies are made from the copied nodes' serializations, then their
    // links are restored together.
    Serialization::NodeSerialization originalSerialization, copySerialization;
    original->toSerialization(&originalSerialization, false);
    copy->toSerialization(&copySerialization, false);
    NodePtr pastedOriginal = createNode(_generatorPluginID);
    NodePtr pastedCopy = createNode(_generatorPluginID);
    ASSERT_TRUE(pastedOriginal && pastedCopy);
    pastedOriginal->fromSerialization(originalSerialization);
    pastedCopy->fromSerialization(copySerialization);
    const std::map<std::string, std::string> noRenames;
    pastedOriginal->storeKnobsLinks(originalSerialization, noRenames);
    pastedCopy->storeKnobsLinks(copySerialization, noRenames);
    NodesList pasted;
    pasted.push_back(pastedOriginal);
    pasted.push_back(pastedCopy);
    KnobHelper::restoreLinkGroups(pasted);

    EXPECT_TRUE(pastedOriginal->isLinkedNode());
    EXPECT_TRUE(pastedCopy->isLinkedNode());
    NodesList linked;
    pastedOriginal->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, pastedCopy));
    EXPECT_FALSE(contains(linked, original));
    EXPECT_FALSE(contains(linked, copy));
    EXPECT_DOUBLE_EQ(0.25, knobOf(pastedCopy)->getValue(0));

    knobOf(pastedCopy)->setValue(0.5, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.5, knobOf(pastedOriginal)->getValue(0));
    EXPECT_DOUBLE_EQ(0.25, knobOf(original)->getValue(0));
    EXPECT_DOUBLE_EQ(0.25, knobOf(copy)->getValue(0));

    knobOf(original)->setValue(0.75, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.75, knobOf(copy)->getValue(0));
    EXPECT_DOUBLE_EQ(0.5, knobOf(pastedCopy)->getValue(0));

    // A single pasted copy has nothing to link to.
    NodePtr single = createNode(_generatorPluginID);
    ASSERT_TRUE(single != nullptr);
    single->fromSerialization(copySerialization);
    single->storeKnobsLinks(copySerialization, noRenames);
    KnobHelper::restoreLinkGroups(NodesList(1, single));
    EXPECT_FALSE(single->isLinkedNode());
    EXPECT_FALSE(knobOf(single)->isLinked(0));
}

TEST_F(LinkedNodeTest, PasteLinksBetweenTracksSurviveASaveAndReload)
{
    NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
    ASSERT_TRUE(bool(tracker));
    TrackerContextPtr context = tracker->getTrackerContext();
    ASSERT_TRUE(bool(context));
    TrackMarkerPtr first = context->createMarker();
    TrackMarkerPtr second = context->createMarker();
    ASSERT_TRUE(first && second);
    ASSERT_TRUE(second->getOffsetKnob()->linkTo(-1, first->getOffsetKnob(), -1));
    first->getOffsetKnob()->setValue(0.25, ViewSpec::all(), 0);
    ASSERT_DOUBLE_EQ(0.25, second->getOffsetKnob()->getValue(0));
    // A track's knob linked to a node's knob is saved under the same group.
    ASSERT_TRUE(knobOf(original)->linkTo(0, first->getOffsetKnob(), 0));
    ASSERT_DOUBLE_EQ(0.25, knobOf(original)->getValue(0));

    const std::string trackerName = tracker->getScriptName();
    const std::string originalName = original->getScriptName();
    const std::string firstName = first->getScriptName_mt_safe();
    const std::string secondName = second->getScriptName_mt_safe();

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("linked-tracks.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    first.reset();
    second.reset();
    context.reset();
    tracker.reset();
    original.reset();
    copy.reset();
    third.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    tracker = project->getNodeByName(trackerName);
    original = project->getNodeByName(originalName);
    ASSERT_TRUE(tracker && original);
    context = tracker->getTrackerContext();
    ASSERT_TRUE(bool(context));
    first = context->getMarkerByName(firstName);
    second = context->getMarkerByName(secondName);
    ASSERT_TRUE(first && second);

    EXPECT_TRUE(first->getOffsetKnob()->isLinked(0));
    EXPECT_TRUE(second->getOffsetKnob()->isLinked(0));
    EXPECT_EQ(first->getOffsetKnob()->getLinkGroup(0), second->getOffsetKnob()->getLinkGroup(0));
    EXPECT_EQ(first->getOffsetKnob()->getLinkGroup(0), knobOf(original)->getLinkGroup(0));
    EXPECT_DOUBLE_EQ(0.25, second->getOffsetKnob()->getValue(0));
    second->getOffsetKnob()->setValue(0.5, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.5, first->getOffsetKnob()->getValue(0));
    EXPECT_DOUBLE_EQ(0.5, knobOf(original)->getValue(0));
}

TEST_F(LinkedNodeTest, APasteLinkInAGroupWithAnAliasSurvivesASaveAndReload)
{
    // third's knob is Paste-Linked to original's, then copy's knob becomes an alias of original's, which
    // makes the group permanent.
    ASSERT_TRUE(knobOf(third)->linkTo(-1, knobOf(original), -1));
    ASSERT_TRUE(knobOf(copy)->setKnobAsAliasOfThis(knobOf(original), true));
    knobOf(original)->setValue(0.25, ViewSpec::all(), 0);
    ASSERT_TRUE(knobOf(third)->isLinkPermanent(0));
    ASSERT_DOUBLE_EQ(0.25, knobOf(third)->getValue(0));

    const std::string originalName = original->getScriptName();
    const std::string copyName = copy->getScriptName();
    const std::string thirdName = third->getScriptName();

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("alias-and-paste-link.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    original.reset();
    copy.reset();
    third.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    original = project->getNodeByName(originalName);
    copy = project->getNodeByName(copyName);
    third = project->getNodeByName(thirdName);
    ASSERT_TRUE(original && copy && third);

    // The alias, its target and the Paste-Linked knob are in one permanent group again.
    ASSERT_TRUE(bool(knobOf(original)->getLinkGroup(0)));
    EXPECT_EQ(KnobIPtr(knobOf(original)), knobOf(copy)->getAliasMaster());
    EXPECT_EQ(knobOf(original)->getLinkGroup(0), knobOf(copy)->getLinkGroup(0));
    EXPECT_EQ(knobOf(original)->getLinkGroup(0), knobOf(third)->getLinkGroup(0));
    EXPECT_TRUE(knobOf(third)->isLinked(0));
    EXPECT_TRUE(knobOf(original)->isLinkPermanent(0));
    EXPECT_TRUE(knobOf(third)->isLinkPermanent(0));
    EXPECT_FALSE(knobOf(third)->unlinkFromGroups());
    EXPECT_DOUBLE_EQ(0.25, knobOf(third)->getValue(0));
    EXPECT_DOUBLE_EQ(0.25, knobOf(copy)->getValue(0));
    knobOf(third)->setValue(0.5, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.5, knobOf(original)->getValue(0));
    EXPECT_DOUBLE_EQ(0.5, knobOf(copy)->getValue(0));
    knobOf(copy)->setValue(0.75, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.75, knobOf(original)->getValue(0));
    EXPECT_DOUBLE_EQ(0.75, knobOf(third)->getValue(0));

    // Without the alias, the Paste Link is an ordinary one again.
    ASSERT_TRUE(knobOf(copy)->setKnobAsAliasOfThis(knobOf(original), false));
    EXPECT_EQ(nullptr, knobOf(copy)->getLinkGroup(0));
    EXPECT_FALSE(knobOf(third)->isLinkPermanent(0));
    EXPECT_EQ(knobOf(original)->getLinkGroup(0), knobOf(third)->getLinkGroup(0));
    EXPECT_TRUE(knobOf(third)->unlinkFromGroups());
    knobOf(original)->setValue(0.125, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.75, knobOf(third)->getValue(0));
}

TEST_F(LinkedNodeTest, OnlyTheTrackerExportOneWayLinksAreRestored)
{
    NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
    ASSERT_TRUE(bool(tracker));
    KnobDoublePtr trackerKnob;
    const KnobsVec& trackerKnobs = tracker->getKnobs();
    for (KnobsVec::const_iterator it = trackerKnobs.begin(); it != trackerKnobs.end(); ++it) {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(*it);
        if (knob && knob->getHolder() == tracker->getEffectInstance().get()) {
            trackerKnob = knob;
            break;
        }
    }
    ASSERT_TRUE(bool(trackerKnob));

    NodesList allNodes;
    getApp()->getProject()->getNodes_recursive(allNodes, true);
    const std::map<std::string, std::string> noRenames;

    // A one-way link to a plain node's knob, which only a hand-edited file holds, is not restored.
    KnobDoublePtr slave = knobOf(copy);
    slave->storeLink(0, original->getFullyQualifiedName(), original->getScriptName(), std::string(), knobName, 0);
    slave->restoreLinks(allNodes, noRenames, false);
    EXPECT_FALSE(slave->isSlave(0));
    EXPECT_FALSE(slave->isLinked(0));

    // The one the Tracker's linked export makes to its own knobs is.
    slave->storeLink(0, tracker->getFullyQualifiedName(), tracker->getScriptName(), std::string(), trackerKnob->getName(), 0);
    slave->restoreLinks(allNodes, noRenames, false);
    EXPECT_TRUE(slave->isSlave(0));
    EXPECT_EQ(KnobIPtr(trackerKnob), slave->getMaster(0).second);
}
