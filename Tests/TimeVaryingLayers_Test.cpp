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
#include <vector>

#include <gtest/gtest.h>

#include <QString>

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
};

// Read(Tests/fixtures/flat-seq-layers.####.exr): frame 1 carries RGBA + diffuse + specular,
// frame 2 carries RGBA only.
TEST_F(TimeVaryingLayersTest, ReadSequenceDiffuseVariesPerFrame)
{
    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-seq-layers.####.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

    EffectInstancePtr effect = reader->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    expectPresentDiffuseAt(effect, 1, 2, true);
    expectPresentDiffuseAt(effect, 2, 1, false);

    // Reverse call order: a cache keyed on "last queried time" rather than the passed time
    // argument would otherwise only be caught by one of the two orderings above.
    expectPresentDiffuseAt(effect, 2, 1, false);
    expectPresentDiffuseAt(effect, 1, 2, true);
}

// OFX Switch (net.sf.openfx.switchPlugin): input 0 is Read(flat-three-layers.exr) (diffuse +
// specular), input 1 is Read(flat-rgba-only.exr) (RGBA only). `which` is keyed 0 at frame 1
// and 1 at frame 2, so the node under test switches streams across the same two frames.
TEST_F(TimeVaryingLayersTest, SwitchDiffuseVariesPerFrame)
{
    CreateNodeArgs readerAArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerAArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    NodePtr readerA = getApp()->createNode(readerAArgs);
    ASSERT_TRUE(bool(readerA)) << "node creation failed for " << _readOIIOPluginID.toStdString();

    CreateNodeArgs readerBArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerBArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
    NodePtr readerB = getApp()->createNode(readerBArgs);
    ASSERT_TRUE(bool(readerB)) << "node creation failed for " << _readOIIOPluginID.toStdString();

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

    expectPresentDiffuseAt(readerA->getEffectInstance(), 1, 2, true);
    expectPresentDiffuseAt(readerA->getEffectInstance(), 2, 1, true);
    expectPresentDiffuseAt(readerB->getEffectInstance(), 1, 2, false);
    expectPresentDiffuseAt(readerB->getEffectInstance(), 2, 1, false);

    EffectInstancePtr effect = switchNode->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    expectPresentDiffuseAt(effect, 1, 2, true);
    expectPresentDiffuseAt(effect, 2, 1, false);

    expectPresentDiffuseAt(effect, 2, 1, false);
    expectPresentDiffuseAt(effect, 1, 2, true);
}

// Read(flat-rgba-only.exr) -> native Shuffle (writing a constant into a new "diffuse" layer) ->
// Dot. The Shuffle's Disable knob (kDisableNodeKnobName) is keyed off at frame 1 and on at
// frame 2, so the downstream Dot sees the Shuffle's diffuse output only at frame 1 and a plain
// passthrough of the RGBA-only reader at frame 2.
TEST_F(TimeVaryingLayersTest, ShuffleDisableDiffuseVariesPerFrame)
{
    ProjectPtr project = getApp()->getProject();
    std::string error;
    const std::vector<std::string> rgb = { "R", "G", "B" };
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("diffuse", "diffuse", "", rgb), LayerRegistryEntry::eOriginUser, &error)) << error;

    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), project);
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

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

    expectPresentDiffuseAt(reader->getEffectInstance(), 1, 2, false);
    expectPresentDiffuseAt(reader->getEffectInstance(), 2, 1, false);

    EffectInstancePtr effect = noop->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    expectPresentDiffuseAt(effect, 1, 2, true);
    expectPresentDiffuseAt(effect, 2, 1, false);

    expectPresentDiffuseAt(effect, 2, 1, false);
    expectPresentDiffuseAt(effect, 1, 2, true);
}
