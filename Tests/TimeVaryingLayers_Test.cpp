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

#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

bool
containsLayer(const std::list<ImageLayerDesc>& layers,
              const std::string& layerID)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->getLayerID() == layerID) {
            return true;
        }
    }

    return false;
}

std::string
layerIDsString(const std::list<ImageLayerDesc>& layers)
{
    std::string result;
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (!result.empty()) {
            result += ", ";
        }
        result += it->getLayerID();
    }

    return "[" + result + "]";
}

bool
containsColorLayer(const std::list<ImageLayerDesc>& layers)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer()) {
            return true;
        }
    }

    return false;
}

} // namespace

// A node's layers are a per-frame fact: the layers its output stream carries at `time`, as
// reported by getPresentLayers(time, view, -1, ...), which is what render-time consumers such as
// Shuffle validate against. getAvailableLayers() is deliberately not used: for the output it also
// merges the project layer registry, which only ever grows, so a layer seen on any frame would be
// reported on every frame by design. Every assertion parks the timeline on the frame *not* being
// queried, so a consumer that reads the timeline's current frame instead of its `time` argument
// fails regardless of which of the two frames the timeline happens to be sitting on.
class TimeVaryingLayersTest
    : public BaseTest {
protected:
    struct TimeVaryingGraph {
        NodePtr underTest;
        // Upstream nodes whose layers are the same on both frames, and whether they carry diffuse:
        // they guard against the node under test passing because its inputs were already wrong.
        std::vector<std::pair<NodePtr, bool>> constantInputs;
    };

    typedef void (TimeVaryingLayersTest::*GraphBuilder)(TimeVaryingGraph*);

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

    void expectPresentDiffuseAt(const EffectInstancePtr& effect,
                                double queryTime,
                                int timelineFrame,
                                bool expectDiffuse)
    {
        getApp()->getTimeLine()->seekFrame(timelineFrame, false, NULL, eTimelineChangeReasonOtherSeek);

        std::list<ImageLayerDesc> present;
        effect->getPresentLayers(queryTime, ViewIdx(0), -1, &present);
        // Guards against a vacuous pass on the "no diffuse" frame when the stream is empty.
        EXPECT_TRUE(containsColorLayer(present))
            << "queryTime=" << queryTime << " timelineFrame=" << timelineFrame << " present=" << layerIDsString(present);
        EXPECT_EQ(expectDiffuse, containsLayer(present, "diffuse"))
            << "queryTime=" << queryTime << " timelineFrame=" << timelineFrame << " present=" << layerIDsString(present);
    }

    // Each query order runs on a graph built from scratch: on a graph an earlier order already
    // queried, every answer could come from a cache filled in that earlier order.
    void expectDiffuseOnlyAtFrame1InEitherQueryOrder(GraphBuilder build)
    {
        for (int firstFrame = 1; firstFrame <= 2; ++firstFrame) {
            SCOPED_TRACE("frame " + std::to_string(firstFrame) + " queried first");
            getApp()->getProject()->reset(false, true);

            TimeVaryingGraph graph;
            (this->*build)(&graph);
            if (HasFatalFailure()) {
                return;
            }
            ASSERT_TRUE(bool(graph.underTest));
            EffectInstancePtr effect = graph.underTest->getEffectInstance();
            ASSERT_TRUE(bool(effect));

            const int secondFrame = 3 - firstFrame;
            expectPresentDiffuseAt(effect, firstFrame, secondFrame, firstFrame == 1);
            expectPresentDiffuseAt(effect, secondFrame, firstFrame, secondFrame == 1);

            for (std::size_t i = 0; i < graph.constantInputs.size(); ++i) {
                EffectInstancePtr input = graph.constantInputs[i].first->getEffectInstance();
                expectPresentDiffuseAt(input, 1, 2, graph.constantInputs[i].second);
                expectPresentDiffuseAt(input, 2, 1, graph.constantInputs[i].second);
            }
        }
    }

    NodePtr createReader(const char* fixturePath)
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(fixturePath));
        NodePtr reader = getApp()->createNode(readerArgs);
        EXPECT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

        return reader;
    }

public:
    // Public because a TEST_F body could only take a protected member's address through the
    // test class TEST_F generates, not through this fixture.

    // Read(Tests/fixtures/flat-seq-layers.####.exr): frame 1 carries RGBA + diffuse + specular,
    // frame 2 carries RGBA only.
    void buildReadSequence(TimeVaryingGraph* graph)
    {
        graph->underTest = createReader(NATRON_TESTS_FIXTURES_DIR "/flat-seq-layers.####.exr");
    }

    // OFX Switch (net.sf.openfx.switchPlugin): input 0 is Read(flat-three-layers.exr) (diffuse +
    // specular), input 1 is Read(flat-rgba-only.exr) (RGBA only). `which` is keyed 0 at frame 1
    // and 1 at frame 2, so the node under test switches streams across the same two frames.
    void buildSwitch(TimeVaryingGraph* graph)
    {
        NodePtr readerA = createReader(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr");
        NodePtr readerB = createReader(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr");
        ASSERT_TRUE(readerA && readerB);

        NodePtr switchNode = createNode(QString::fromUtf8("net.sf.openfx.switchPlugin"));
        ASSERT_TRUE(bool(switchNode));

        connectNodes(readerA, switchNode, 0, true);
        connectNodes(readerB, switchNode, 1, true);

        KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName("which"));
        ASSERT_TRUE(bool(which));
        which->setValueAtTime(1, 0, ViewSpec::all(), 0);
        which->setValueAtTime(2, 1, ViewSpec::all(), 0);
        EXPECT_EQ(0, which->getValueAtTime(1));
        EXPECT_EQ(1, which->getValueAtTime(2));

        graph->underTest = switchNode;
        graph->constantInputs.push_back(std::make_pair(readerA, true));
        graph->constantInputs.push_back(std::make_pair(readerB, false));
    }

    // Read(flat-rgba-only.exr) -> native Shuffle (writing a constant into a new "diffuse" layer)
    // -> Dot. The Shuffle's Disable is keyed off at frame 1 and on at frame 2, so the Dot sees the
    // Shuffle's diffuse output only at frame 1 and a plain passthrough of the reader at frame 2.
    void buildShuffleDisableChain(TimeVaryingGraph* graph)
    {
        ProjectPtr project = getApp()->getProject();
        std::string error;
        const std::vector<std::string> rgb = { "R", "G", "B" };
        ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("diffuse", "diffuse", "", rgb), LayerRegistryEntry::eOriginUser, &error)) << error;

        NodePtr reader = createReader(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr");
        ASSERT_TRUE(bool(reader));

        NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
        ASSERT_TRUE(bool(shuffle));
        connectNodes(reader, shuffle, Shuffle::eInputMain, true);

        KnobLayerSelectPtr out1 = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(kShuffleParamOut1));
        ASSERT_TRUE(bool(out1));
        out1->setLayer("diffuse");

        KnobShuffleMapPtr mapping = std::dynamic_pointer_cast<KnobShuffleMap>(shuffle->getKnobByName(kShuffleParamMapping));
        ASSERT_TRUE(bool(mapping));
        mapping->setSource(1, 0, ShuffleSource::makeOne());
        mapping->setSource(1, 1, ShuffleSource::makeOne());
        mapping->setSource(1, 2, ShuffleSource::makeOne());

        NodePtr noop = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
        ASSERT_TRUE(bool(noop));
        connectNodes(shuffle, noop, 0, true);

        KnobBoolPtr disable = std::dynamic_pointer_cast<KnobBool>(shuffle->getKnobByName(kDisableNodeKnobName));
        ASSERT_TRUE(bool(disable));
        disable->setValueAtTime(1, false, ViewSpec::all(), 0);
        disable->setValueAtTime(2, true, ViewSpec::all(), 0);
        EXPECT_FALSE(disable->getValueAtTime(1));
        EXPECT_TRUE(disable->getValueAtTime(2));

        graph->underTest = noop;
        graph->constantInputs.push_back(std::make_pair(reader, false));
    }
};

TEST_F(TimeVaryingLayersTest, ReadSequenceDiffuseVariesPerFrame)
{
    expectDiffuseOnlyAtFrame1InEitherQueryOrder(&TimeVaryingLayersTest::buildReadSequence);
}

TEST_F(TimeVaryingLayersTest, SwitchDiffuseVariesPerFrame)
{
    expectDiffuseOnlyAtFrame1InEitherQueryOrder(&TimeVaryingLayersTest::buildSwitch);
}

TEST_F(TimeVaryingLayersTest, ShuffleDisableDiffuseVariesPerFrame)
{
    expectDiffuseOnlyAtFrame1InEitherQueryOrder(&TimeVaryingLayersTest::buildShuffleDisableChain);
}

TEST_F(TimeVaryingLayersTest, KeyedDisableSurvivesSaveLoad)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    const std::string nodeName = node->getScriptName();

    KnobBoolPtr disable = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kDisableNodeKnobName));
    ASSERT_TRUE(bool(disable));
    disable->setValueAtTime(1, false, ViewSpec::all(), 0);
    disable->setValueAtTime(2, true, ViewSpec::all(), 0);
    ASSERT_EQ(2, disable->getKeyFramesCount(ViewSpec::all(), 0));

    EXPECT_FALSE(node->isNodeDisabled(1.0));
    EXPECT_TRUE(node->isNodeDisabled(2.0));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("keyed-disable-roundtrip.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->getNodeByName(nodeName).get() == NULL);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr node2 = project->getNodeByName(nodeName);
    ASSERT_TRUE(bool(node2));

    KnobBoolPtr disable2 = std::dynamic_pointer_cast<KnobBool>(node2->getKnobByName(kDisableNodeKnobName));
    ASSERT_TRUE(bool(disable2));
    EXPECT_EQ(2, disable2->getKeyFramesCount(ViewSpec::all(), 0));
    EXPECT_FALSE(disable2->getValueAtTime(1));
    EXPECT_TRUE(disable2->getValueAtTime(2));

    EXPECT_FALSE(node2->isNodeDisabled(1.0));
    EXPECT_TRUE(node2->isNodeDisabled(2.0));
}
