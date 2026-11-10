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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/GraphEditRecorder.h"
#include "Engine/GraphOpApply.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"

#include "Serialization/GraphOpTestHelpers.h"
#include "Serialization/Ops/ApplyOps.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_USING
using namespace Serialization;

class GraphEditReplayTest
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

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    NodePtr createIn(const char* pluginID,
                     const NodeCollectionPtr& parent)
    {
        CreateNodeArgs args(pluginID, parent);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);

        return getApp()->createNode(args);
    }

    static KnobDoubleBasePtr doubleKnob(const NodePtr& node,
                                        const char* name)
    {
        return std::dynamic_pointer_cast<KnobDoubleBase>(node->getKnobByName(name));
    }

    ProjectSerialization snapshot() const
    {
        ProjectSerialization serialization;
        project()->toSerialization(&serialization);

        return serialization;
    }
};

TEST_F(GraphEditReplayTest, ReplayingRecordedEditsOnTheSavedProjectGivesTheEditedProject)
{
    NodePtr constant = createIn(PLUGINID_NATRON_CONSTANT, project());
    NodePtr grade = createIn(PLUGINID_NATRON_GRADE, project());
    NodePtr merge = createIn(PLUGINID_NATRON_NATIVE_MERGE, project());
    NodePtr rotoPaint = createIn(PLUGINID_NATRON_ROTOPAINT, project());
    NodePtr tracker = createIn(PLUGINID_NATRON_TRACKER, project());
    NodePtr looseBlur = createIn(PLUGINID_NATRON_BLUR, project());
    NodePtr group = createIn(PLUGINID_NATRON_GROUP, project());
    ASSERT_NE(constant, nullptr);
    ASSERT_NE(grade, nullptr);
    ASSERT_NE(merge, nullptr);
    ASSERT_NE(rotoPaint, nullptr);
    ASSERT_NE(tracker, nullptr);
    ASSERT_NE(looseBlur, nullptr);
    ASSERT_NE(group, nullptr);
    NodeGroupPtr groupCollection = std::dynamic_pointer_cast<NodeGroup>(group->getEffectInstance());
    ASSERT_NE(groupCollection, nullptr);
    NodePtr innerBlur = createIn(PLUGINID_NATRON_BLUR, groupCollection);
    ASSERT_NE(innerBlur, nullptr);
    // Replay re-creates a deleted then reactivated node at the end of its collection, so the
    // original must already be last there.
    NodePtr dot = createIn(PLUGINID_NATRON_DOT, project());
    ASSERT_NE(dot, nullptr);

    ASSERT_TRUE(NodeCollection::connectNodes(0, constant, grade, false));
    ASSERT_TRUE(NodeCollection::connectNodes(0, grade, merge, false));
    ASSERT_TRUE(NodeCollection::connectNodes(1, constant, merge, false));
    ASSERT_TRUE(NodeCollection::connectNodes(0, constant, dot, false));
    ASSERT_TRUE(NodeCollection::connectNodes(0, dot, rotoPaint, false));
    ASSERT_TRUE(NodeCollection::connectNodes(0, grade, tracker, false));
    ASSERT_TRUE(NodeCollection::connectNodes(0, constant, looseBlur, false));
    double y = 0.;
    for (const NodePtr& node : { constant, grade, merge, rotoPaint, tracker, looseBlur, group, dot }) {
        node->setPosition(0., y);
        y += 100.;
    }

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("replay-s0.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project()->saveProject(dirPath, fileName, &savedFilePath));
    const ProjectSerialization s0 = snapshot();

    std::vector<GraphOp> recorded;
    GraphEditRecorder* recorder = project()->getGraphEditRecorder();
    const int subscription = recorder->subscribe([&recorded](const std::vector<GraphOp>& ops) {
        recorded.insert(recorded.end(), ops.begin(), ops.end());
    });

    dot->deactivate(NodesList(), true, false, true, false);
    recorder->flush();
    dot->activate(NodesList(), true, false);
    recorder->flush();

    doubleKnob(grade, kGradeParamMultiply)->setValue(1.5, ViewSpec::all(), 0);
    doubleKnob(grade, kGradeParamGamma)->setValueAtTime(1., 0.8, ViewSpec::all(), 0);
    doubleKnob(grade, kGradeParamGamma)->setValueAtTime(20., 1.2, ViewSpec::all(), 0);
    doubleKnob(grade, kGradeParamWhite)->setExpression(0, "1 + 0.5", false, true);
    doubleKnob(constant, kConstantParamColor)->setValue(0.25, ViewSpec::all(), 1);
    doubleKnob(constant, kConstantParamColor)->setValue(0.75, ViewSpec::all(), 2);
    doubleKnob(constant, kConstantParamColor)->resetToDefaultValue(1);
    doubleKnob(innerBlur, kBlurParamSize)->setValue(7., ViewSpec::all(), 0);
    recorder->flush();

    merge->setScriptName("MergeRenamed");
    grade->setLabel("Graded");
    merge->disconnectInput(1);
    ASSERT_TRUE(NodeCollection::connectNodes(1, dot, merge, false));
    for (int i = 0; i < 20; ++i) {
        merge->setPosition(10. * i, 200. + i);
    }
    grade->setColor(0.2, 0.4, 0.6);
    innerBlur->setSize(120., 40.);
    recorder->flush();

    RotoContextPtr roto = rotoPaint->getRotoContext();
    ASSERT_NE(roto, nullptr);
    RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, roto, "stroke", RotoLayerPtr()));
    stroke->createNodes();
    roto->addItem(roto->getOrCreateBaseLayer(), 0, stroke, RotoItem::eSelectionReasonOther);
    for (int i = 0; i < 12; ++i) {
        stroke->appendPoint(i == 0, RotoPoint(5. + 3. * i, 8. + i, 1., i));
    }
    stroke->setStrokeFinished();

    TrackerContextPtr tracks = tracker->getTrackerContext();
    ASSERT_NE(tracks, nullptr);
    TrackMarkerPtr marker = tracks->createMarker();
    ASSERT_NE(marker, nullptr);
    marker->setLabel("Corner");

    EffectInstancePtr gradeEffect = grade->getEffectInstance();
    KnobDoublePtr userKnob = gradeEffect->createDoubleKnob("strength", "Strength", 1);
    ASSERT_NE(userKnob, nullptr);
    gradeEffect->getOrCreateUserPageKnob()->addKnob(userKnob);
    userKnob->setValue(0.5, ViewSpec::all(), 0);

    KnobGroupPtr variables = project()->getRFVGroup();
    ASSERT_NE(variables, nullptr);
    KnobDoublePtr gain = project()->createDoubleKnob("rfvGain", "Gain", 1);
    ASSERT_TRUE(variables->addKnob(gain));
    KnobDoublePtr offset = project()->createDoubleKnob("rfvOffset", "Offset", 1);
    ASSERT_TRUE(variables->addKnob(offset));
    gain->setValue(2., ViewSpec::all(), 0);
    ASSERT_TRUE(variables->moveOneStepUp(offset.get()));

    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded,
              project()->addLayer(ImageLayerDesc("mask", "mask", "", std::vector<std::string>(1, "A")), LayerRegistryEntry::eOriginUser, &error))
        << error;
    recorder->flush();

    // Group from selection, as primitive edits: a new group, a copy inside it, the original gone.
    NodePtr selectionGroup = createIn(PLUGINID_NATRON_GROUP, project());
    ASSERT_NE(selectionGroup, nullptr);
    NodeGroupPtr selectionCollection = std::dynamic_pointer_cast<NodeGroup>(selectionGroup->getEffectInstance());
    ASSERT_NE(selectionCollection, nullptr);
    NodePtr blurCopy = createIn(PLUGINID_NATRON_BLUR, selectionCollection);
    ASSERT_NE(blurCopy, nullptr);
    doubleKnob(blurCopy, kBlurParamSize)->setValue(3., ViewSpec::all(), 0);
    looseBlur->deactivate(NodesList(), true, false, true, false);
    recorder->flush();

    NodePtr extra = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(extra, nullptr);
    ASSERT_TRUE(NodeCollection::connectNodes(0, grade, extra, false));
    extra->setScriptName("Extra");
    doubleKnob(extra, kBlurParamSize)->setValue(2., ViewSpec::all(), 1);
    NodePtr transient = createIn(PLUGINID_NATRON_DOT, project());
    ASSERT_NE(transient, nullptr);
    transient->destroyNode(true, false);
    recorder->flush();

    const ProjectSerialization s1 = snapshot();
    recorder->unsubscribe(subscription);
    ASSERT_FALSE(recorded.empty());

    ProjectSerialization diffed = s0;
    for (const OpResult& result : applyOps(&diffed, diffSnapshots(s0, s1))) {
        EXPECT_TRUE(result.applied()) << result.message;
    }
    EXPECT_SAME_PROJECT(s1, diffed);

    project()->reset(false, true);
    ASSERT_TRUE(project()->loadProject(dirPath, fileName));
    EXPECT_SAME_PROJECT(s0, snapshot());
    const std::vector<OpResult> results = applyGraphOps(project(), recorded);
    ASSERT_EQ(results.size(), recorded.size());
    for (std::size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].applied()) << "operation " << i << ": " << results[i].message;
    }
    EXPECT_EQ(GraphOpTest::editableText(s1), GraphOpTest::editableText(snapshot()));
}
