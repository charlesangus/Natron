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
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// A 16x16 RGBA pattern, so the resample and the R/B swap have varying values to get wrong.
const char* const kPatternFixture = "flat-rgba-pattern.exr";

// RGBA plus the diffuse and specular layers, each constant over 8x8.
const char* const kLayersFixture = "flat-three-layers.exr";
const char* const kOtherLayer = "specular";

struct Chain {
    NodePtr read;
    NodePtr reformat;
    NodePtr grade;
    NodePtr shuffle;
    NodePtr write;
};

int
gpuRunsOf(const EndToEndResult& result,
          const NodePtr& node)
{
    const std::map<std::string, int>::const_iterator found = result.gpuTasksPerNode.find(node->getScriptName_mt_safe());

    return (found == result.gpuTasksPerNode.end()) ? 0 : found->second;
}

bool
setLayer(const NodePtr& node,
         const char* knobName,
         const std::string& layerID)
{
    KnobLayerSelectPtr knob = std::dynamic_pointer_cast<KnobLayerSelect>(node->getKnobByName(knobName));

    EXPECT_TRUE(bool(knob)) << knobName;
    if (!knob) {
        return false;
    }
    knob->setLayer(layerID);

    return true;
}

RectI
windowOf(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    const StatusEnum st = effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);

    if ((st == eStatusFailed) || rod.isNull()) {
        return RectI();
    }

    return rod.toPixelEnclosing(0, 1.);
}

// The region holding any of the chain's three processing nodes must be the only one, and hold only them.
void
expectOneRegionOfTheChain(const EndToEndResult& result,
                          const Chain& chain)
{
    ASSERT_EQ(1u, result.regions.size());
    ASSERT_EQ(1u, result.regionNodes.size());
    const std::vector<NodePtr>& nodes = result.regionNodes.front();
    for (const NodePtr& member : { chain.reformat, chain.grade, chain.shuffle }) {
        EXPECT_NE(nodes.end(), std::find(nodes.begin(), nodes.end(), member)) << member->getScriptName_mt_safe() << " is not in the region";
    }
    for (const NodePtr& node : nodes) {
        EXPECT_TRUE((node == chain.reformat) || (node == chain.grade) || (node == chain.shuffle)) << node->getScriptName_mt_safe() << " is in the region";
    }
    EXPECT_FALSE(result.ranInRegion(chain.read));
    EXPECT_FALSE(result.ranInRegion(chain.write));

    EXPECT_EQ(1, gpuRunsOf(result, chain.reformat)) << "the Reformat fell back to the CPU";
    EXPECT_EQ(1, gpuRunsOf(result, chain.grade)) << "the Grade fell back to the CPU";
    EXPECT_EQ(1, gpuRunsOf(result, chain.shuffle)) << "the Shuffle fell back to the CPU";
    EXPECT_EQ(0, gpuRunsOf(result, chain.read));
}

void
expectMatchesCpu(const EndToEndResult& result)
{
    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    bool nonZero = false;
    for (float v : result.cpu) {
        nonZero = nonZero || (v != 0.f);
    }
    EXPECT_TRUE(nonZero) << "the CPU render is black, which proves nothing";
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::resampling());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ": CPU " << result.cpu[diff.firstMismatchIndex] << ", GPU "
                                   << result.gpu[diff.firstMismatchIndex] << "; worst " << diff.worstAbsolute;
}

} // namespace

class NativeGpuChainTest
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

    // Read → Reformat → Grade → Shuffle → Write. The Reformat distorts to a box whose size is no integer multiple of
    // the fixture's, so every output sample is interpolated, and the Shuffle swaps R and B and writes alpha 1, so it is
    // no route identity.
    void makeChain(const char* fixture,
                   Chain* chain)
    {
        chain->read = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);
        chain->reformat = createNode(QString::fromUtf8(PLUGINID_NATRON_REFORMAT), PLUGIN_MAJOR_NATRON_REFORMAT);
        chain->grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        chain->shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
        chain->write = createNativeWriter();
        ASSERT_TRUE(chain->read && chain->reformat && chain->grade && chain->shuffle && chain->write);
        connectNodes(chain->read, chain->reformat, 0, true);
        connectNodes(chain->reformat, chain->grade, 0, true);
        connectNodes(chain->grade, chain->shuffle, Shuffle::eInputMain, true);
        connectNodes(chain->shuffle, chain->write, 0, true);

        ASSERT_TRUE(setKnobValue(chain->reformat, kReformatParamType, std::string(kReformatParamTypeOptionToBox)));
        ASSERT_TRUE(setKnobValues(chain->reformat, kReformatParamBoxSize, { 27., 21. }));
        ASSERT_TRUE(setKnobValues(chain->reformat, kReformatParamBoxFixed, { 1. }));
        ASSERT_TRUE(setKnobValue(chain->reformat, kReformatParamResize, std::string(kReformatParamResizeOptionDistort)));
        ASSERT_TRUE(setKnobValue(chain->reformat, kResamplerParamFilterType, std::string("keys")));

        ASSERT_TRUE(setKnobValues(chain->grade, kGradeParamMultiply, { 1.5, 0.75, 1.25, 1. }));
        ASSERT_TRUE(setKnobValues(chain->grade, kGradeParamOffset, { 0.05, 0.1, 0., 0. }));

        ASSERT_TRUE(setLayer(chain->shuffle, kShuffleParamIn1, kNatronColorViewRGBA));
        ASSERT_TRUE(setLayer(chain->shuffle, kShuffleParamIn2, std::string()));
        ASSERT_TRUE(setLayer(chain->shuffle, kShuffleParamOut1, kNatronColorViewRGBA));
        ASSERT_TRUE(setLayer(chain->shuffle, kShuffleParamOut2, std::string()));
        KnobShuffleMapPtr mapping = std::dynamic_pointer_cast<KnobShuffleMap>(chain->shuffle->getKnobByName(kShuffleParamMapping));
        ASSERT_TRUE(bool(mapping));
        mapping->setSource(1, 0, ShuffleSource::makeInput(1, 2));
        mapping->setSource(1, 2, ShuffleSource::makeInput(1, 0));
        mapping->setSource(1, 3, ShuffleSource::makeOne());
    }

    // Every node is offered to the GPU, so the Read and the Write staying out of the region is the placement's answer,
    // not the test's.
    EndToEndResult render(const Chain& chain)
    {
        const RectI window = windowOf(chain.shuffle);
        EXPECT_FALSE(window.isNull());

        return renderEndToEnd(std::vector<NodePtr>({ chain.read, chain.reformat, chain.grade, chain.shuffle, chain.write }), chain.shuffle, window, kTime);
    }
};

TEST_F(NativeGpuChainTest, ReformatGradeShuffleRunAsOneRegionBetweenTheReadAndTheWrite)
{
    Chain chain;
    makeChain(kPatternFixture, &chain);
    if (HasFatalFailure()) {
        return;
    }

    const EndToEndResult result = render(chain);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    expectOneRegionOfTheChain(result, chain);
    expectMatchesCpu(result);
}

// The Shuffle's green comes from another layer of the Read, which the Reformat and the Grade only pass through, so
// they hold that plane as identity members on the Read's upload and the chain stays one region.
TEST_F(NativeGpuChainTest, ShuffleReadingAnotherLayerOfTheReadKeepsOneRegion)
{
    Chain chain;
    makeChain(kLayersFixture, &chain);
    if (HasFatalFailure()) {
        return;
    }
    ASSERT_TRUE(setLayer(chain.shuffle, kShuffleParamIn2, kOtherLayer));
    KnobShuffleMapPtr mapping = std::dynamic_pointer_cast<KnobShuffleMap>(chain.shuffle->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(mapping));
    mapping->setSource(1, 1, ShuffleSource::makeInput(2, 2));

    const EndToEndResult result = render(chain);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    expectOneRegionOfTheChain(result, chain);
    expectMatchesCpu(result);

    // The fixture's specular is (0, 0, 1), so a green that is black everywhere means the layer was never read.
    bool greenFromLayer = false;
    for (std::size_t i = 1; i < result.cpu.size(); i += 4) {
        greenFromLayer = greenFromLayer || (result.cpu[i] > 0.5f);
    }
    EXPECT_TRUE(greenFromLayer) << "the CPU render's green does not come from the " << kOtherLayer << " layer";
}

// A frame the region delivers retires the channel-selector error an older render posted, as renderRoI() does for a
// frame the CPU delivers, so scrubbing back to a good frame clears the error whichever device renders it.
TEST_F(NativeGpuChainTest, ShuffleDeliveredByTheRegionRetiresAnOlderChannelError)
{
    Chain chain;
    makeChain(kPatternFixture, &chain);
    if (HasFatalFailure()) {
        return;
    }
    chain.shuffle->setChannelSelectorMessage("channel error of an older render");
    ASSERT_TRUE(chain.shuffle->hasPersistentMessage());

    bool errorAfterGpuFrame = true;
    const GpuFrameDone readError = [&]() {
        errorAfterGpuFrame = chain.shuffle->hasPersistentMessage();
    };
    const RectI window = windowOf(chain.shuffle);
    ASSERT_FALSE(window.isNull());
    const std::vector<NodePtr> nodes({ chain.read, chain.reformat, chain.grade, chain.shuffle, chain.write });
    const EndToEndResult result = renderEndToEnd(nodes, chain.shuffle, window, kTime, GpuContextReady(), readError);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(1, gpuRunsOf(result, chain.shuffle)) << "the Shuffle fell back to the CPU, whose renderRoI() retires the error itself";
    EXPECT_FALSE(errorAfterGpuFrame) << "a frame the GPU delivered left the older render's channel error";
}
