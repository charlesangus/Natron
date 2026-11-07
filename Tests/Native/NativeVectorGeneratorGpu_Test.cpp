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

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"
#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/Native/NativeVectorGeneratorTest.h"
#include "Tests/NativeParity.h"

#include "Engine/AppManager.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/GpuContext.h"
#endif
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Time/VectorGenerator.h"

NATRON_NAMESPACE_ENTER

using namespace NativeVectorGeneratorPlate;

namespace {
// The mean distance between the two paths' vectors, and between their confidences.
constexpr double kVectorTolerance = 0.05;
constexpr double kConfidenceTolerance = 1e-3;

// A pool too small for the solve's scratch once the three frames and the output are on the device.
constexpr std::uint64_t kImagesInTightBudget = 4;

constexpr double kTime = 3.;

// Most of the solve is still to run when the abort comes.
constexpr int kBatchesBeforeAbort = 3;
constexpr double kAbortBoundMs = 2000.;

// The output pass of a plane and the segment boundary it brings, with room for a pass the solve would add.
constexpr int kBatchesOfOutputPlane = 8;

// One layer's two channels from the RGBA a Shuffle wrote them to.
RenderedPlane
twoChannelsOf(const std::vector<float>& rgba,
              const RectI& window,
              int firstChannel)
{
    RenderedPlane plane;
    plane.window = window;
    plane.channels = { "0", "1" };
    plane.pixels.reserve(rgba.size() / 2);
    for (std::size_t i = 0; (i + 3) < rgba.size(); i += 4) {
        plane.pixels.push_back(rgba[i + firstChannel]);
        plane.pixels.push_back(rgba[i + firstChannel + 1]);
    }

    return plane;
}

double
meanVectorDistance(const RenderedPlane& a,
                   const RenderedPlane& b)
{
    EXPECT_EQ(a.pixels.size(), b.pixels.size());
    if (a.pixels.empty() || (a.pixels.size() != b.pixels.size())) {
        return 1e30;
    }
    double sum = 0.;
    for (std::size_t i = 0; (i + 1) < a.pixels.size(); i += 2) {
        sum += std::hypot(a.pixels[i] - b.pixels[i], a.pixels[i + 1] - b.pixels[i + 1]);
    }

    return sum / ((double)a.pixels.size() / 2.);
}

double
meanChannelDifference(const std::vector<float>& a,
                      const std::vector<float>& b)
{
    EXPECT_EQ(a.size(), b.size());
    if (a.empty() || (a.size() != b.size())) {
        return 1e30;
    }
    double sum = 0.;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += std::fabs((double)a[i] - b[i]);
    }

    return sum / (double)a.size();
}

int
gpuRunsOf(const EndToEndResult& result,
          const NodePtr& node)
{
    const std::map<std::string, int>::const_iterator found = result.gpuTasksPerNode.find(node->getScriptName_mt_safe());

    return (found == result.gpuTasksPerNode.end()) ? 0 : found->second;
}

// The forward and backward vectors the GPU path gave.
struct RenderedFlow {
    RenderedPlane forward;
    RenderedPlane backward;
};
} // anonymous namespace

class NativeVectorGeneratorGpuTest
    : public NativeTimeNodeTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerMovingTextureOnce();
        NativeTimeNodeTest::SetUp();
    }

    NodePtr createMovingTexture()
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMovingTexture), 1);
        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr createGenerator(const NodePtr& source)
    {
        return createOver(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, source);
    }

    // A Shuffle on the CPU that writes first's two channels to red and green, second's to blue and alpha. Only the
    // generator is offered the GPU, so its planes are the ones the region downloads and this reads.
    NodePtr createReader(const NodePtr& generator,
                         const std::string& first,
                         const std::string& second)
    {
        NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
        EXPECT_TRUE(bool(shuffle));
        if (!shuffle) {
            return NodePtr();
        }
        connectNodes(generator, shuffle, Shuffle::eInputMain, true);
        const char* knobs[] = { kShuffleParamIn1, kShuffleParamIn2, kShuffleParamOut1, kShuffleParamOut2 };
        const std::string layers[] = { first, second, kNatronColorViewRGBA, std::string() };
        for (int i = 0; i < 4; ++i) {
            KnobLayerSelectPtr knob = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(knobs[i]));
            EXPECT_TRUE(bool(knob)) << knobs[i];
            if (!knob) {
                return NodePtr();
            }
            knob->setLayer(layers[i]);
        }
        KnobShuffleMapPtr mapping = std::dynamic_pointer_cast<KnobShuffleMap>(shuffle->getKnobByName(kShuffleParamMapping));
        EXPECT_TRUE(bool(mapping));
        if (!mapping) {
            return NodePtr();
        }
        mapping->setSource(1, 0, ShuffleSource::makeInput(1, 0));
        mapping->setSource(1, 1, ShuffleSource::makeInput(1, 1));
        mapping->setSource(1, 2, second.empty() ? ShuffleSource::makeZero() : ShuffleSource::makeInput(2, 0));
        mapping->setSource(1, 3, second.empty() ? ShuffleSource::makeZero() : ShuffleSource::makeInput(2, 1));

        return shuffle;
    }

    // Renders reader on both paths, the generator on the GPU. False when there is no device or the frame failed.
    bool renderThrough(const NodePtr& generator,
                       const NodePtr& reader,
                       const RectI& window,
                       double time,
                       unsigned mipmapLevel,
                       EndToEndResult* result,
                       bool onGpu = true)
    {
        *result = renderEndToEnd(std::vector<NodePtr>({ generator }), reader, window, time, GpuContextReady(), GpuFrameDone(), mipmapLevel);
        if (result->skipped) {
            return false;
        }
        EXPECT_TRUE(result->ok) << result->error;
        if (!result->ok) {
            return false;
        }
        if (onGpu) {
            EXPECT_TRUE(result->ranInRegion(generator)) << "the node was not placed in a GPU region";
            EXPECT_GT(gpuRunsOf(*result, generator), 0) << "the node fell back to the CPU";
        } else {
            EXPECT_EQ(0, gpuRunsOf(*result, generator)) << "the node rendered on the GPU";
        }

        return true;
    }

    // The forward and backward vectors on both paths, which must agree.
    bool renderVectors(const NodePtr& generator,
                       double time,
                       RenderedFlow* flow,
                       unsigned mipmapLevel = 0)
    {
        const RectI window = windowAt(mipmapLevel);
        NodePtr reader = createReader(generator, kNatronForwardMotionVectorsLayerID, kNatronBackwardMotionVectorsLayerID);
        EndToEndResult result;
        if (!reader || !renderThrough(generator, reader, window, time, mipmapLevel, &result)) {
            return false;
        }
        flow->forward = twoChannelsOf(result.gpu, window, 0);
        flow->backward = twoChannelsOf(result.gpu, window, 2);
        EXPECT_LT(meanVectorDistance(flow->forward, twoChannelsOf(result.cpu, window, 0)), kVectorTolerance);
        EXPECT_LT(meanVectorDistance(flow->backward, twoChannelsOf(result.cpu, window, 2)), kVectorTolerance);

        return true;
    }

    // The one direction a node outputs, on both paths, which must agree.
    void expectDirectionMatchesTruth(const NodePtr& generator,
                                     const std::string& layer,
                                     double a,
                                     double b)
    {
        const RectI window = windowAt(0);
        NodePtr reader = createReader(generator, layer, std::string());
        ASSERT_TRUE(bool(reader));
        EndToEndResult result;
        if (renderThrough(generator, reader, window, kTime, 0, &result)) {
            const RenderedPlane gpu = twoChannelsOf(result.gpu, window, 0);
            EXPECT_LT(meanVectorDistance(gpu, twoChannelsOf(result.cpu, window, 0)), kVectorTolerance);
            EXPECT_LT(meanDistance(gpu, a, b, kMargin), kDisEpe);
        }
    }

    // The confidence plane on both paths, which must agree; the GPU's comes back in `confidence`.
    bool renderConfidence(const NodePtr& generator,
                          double time,
                          RenderedPlane* confidence,
                          unsigned mipmapLevel = 0)
    {
        const RectI window = windowAt(mipmapLevel);
        NodePtr reader = createReader(generator, kVectorGeneratorConfidenceLayerID, std::string());
        EndToEndResult result;
        if (!reader || !renderThrough(generator, reader, window, time, mipmapLevel, &result)) {
            return false;
        }
        *confidence = twoChannelsOf(result.gpu, window, 0);
        EXPECT_LT(meanChannelDifference(confidence->pixels, twoChannelsOf(result.cpu, window, 0).pixels), kConfidenceTolerance);

        return true;
    }

    // The node stays on the CPU, whose result the frame carries through the scheduler unchanged.
    void expectStaysOnCpu(const NodePtr& generator,
                          double time)
    {
        NodePtr reader = createReader(generator, kNatronForwardMotionVectorsLayerID, kNatronBackwardMotionVectorsLayerID);
        ASSERT_TRUE(bool(reader));
        EndToEndResult result;
        if (!renderThrough(generator, reader, windowAt(0), time, 0, &result, false)) {
            return;
        }
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }

    // The frame fails on the GPU path as it does on the CPU's. False when there is no device.
    bool expectFrameFails(const NodePtr& generator,
                          double time)
    {
        NodePtr reader = createReader(generator, kNatronForwardMotionVectorsLayerID, kNatronBackwardMotionVectorsLayerID);
        EXPECT_TRUE(bool(reader));
        if (!reader) {
            return false;
        }
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ generator }), reader, windowAt(0), time);
        if (result.skipped) {
            return false;
        }
        EXPECT_FALSE(result.ok) << "the frame rendered";
        EXPECT_EQ(0, gpuRunsOf(result, generator));

        return true;
    }

    // A pool of `budget` bytes leaves the node to the CPU for want of device memory, and the frame still renders it.
    void expectFallsBackWithBudget(std::uint64_t budget)
    {
        NodePtr generator = createGenerator(createMovingTexture());
        ASSERT_TRUE(bool(generator));
        NodePtr reader = createReader(generator, kNatronForwardMotionVectorsLayerID, kNatronBackwardMotionVectorsLayerID);
        ASSERT_TRUE(bool(reader));
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ generator }), reader, windowAt(0), kTime, [budget](std::string*) {
#ifdef NATRON_HAVE_GPU
            appPTR->getGpuContext()->runtime()->pool().setUserBudget(budget);
#else
            Q_UNUSED(budget);
#endif

            return true;
        });
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_EQ(0, gpuRunsOf(result, generator));
        const std::map<std::string, int>::const_iterator outOfMemory = result.cpuFallbackReasons.find("out of device memory");
        EXPECT_TRUE(outOfMemory != result.cpuFallbackReasons.end()) << "the node did not fall back for want of device memory";
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
        EXPECT_LT(meanDistance(twoChannelsOf(result.gpu, windowAt(0), 0), kShiftX, kShiftY, kMargin), kDisEpe);
    }

#ifdef NATRON_HAVE_GPU
    void expectAbortLeavesNothingBehind(const std::string& method)
    {
        NodePtr generator = createGenerator(createMovingTexture());
        ASSERT_TRUE(bool(generator));
        ASSERT_TRUE(setKnobValue(generator, kVectorGeneratorParamMethod, method));
        NodePtr reader = createReader(generator, kNatronForwardMotionVectorsLayerID, kNatronBackwardMotionVectorsLayerID);
        ASSERT_TRUE(bool(reader));
        const AbortedSolveResult result = renderWithAbortMidSolve(std::vector<NodePtr>({ generator }), reader, windowAt(0), kTime, kBatchesBeforeAbort);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.error.empty()) << result.error;

        EXPECT_TRUE(result.abortFired);
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, result.abortedCode);
        EXPECT_FALSE(result.abortedHasPlanes);
        EXPECT_TRUE(result.abortedFallbacks.empty()) << "the abort moved work to the CPU";
        EXPECT_EQ(0, result.batchesAfterAbort) << "work ran after the abort";
        EXPECT_LT(result.abortToEndMs, kAbortBoundMs);
        EXPECT_LE(result.liveAfterAbort, result.liveAfterClean) << "the aborted solve still holds pool buffers";

        EXPECT_GT(result.splitBatches, kBatchesBeforeAbort * 4);
        const std::map<std::string, int>::const_iterator ran = result.splitGpuTasksPerNode.find(generator->getScriptName_mt_safe());
        EXPECT_TRUE((ran != result.splitGpuTasksPerNode.end()) && (ran->second > 0)) << "the aborted solve's image was cached";
        const ImageDiff diff = compareImages(result.unsplit, result.split, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
#endif // NATRON_HAVE_GPU

    void expectVectorsMatchTruth(const NodePtr& generator,
                                 double threshold)
    {
        RenderedFlow flow;
        if (renderVectors(generator, kTime, &flow)) {
            EXPECT_LT(meanDistance(flow.forward, kShiftX, kShiftY, kMargin), threshold);
            EXPECT_LT(meanDistance(flow.backward, -kShiftX, -kShiftY, kMargin), threshold);
        }
    }
};

TEST_F(NativeVectorGeneratorGpuTest, DisVectorsMatchTheMotionAndTheCpu)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kVectorGeneratorParamMethod, kVectorGeneratorMethodDIS));
    expectVectorsMatchTruth(node, kDisEpe);

    RenderedPlane confidence;
    if (renderConfidence(node, kTime, &confidence)) {
        EXPECT_GT(meanOfChannel(confidence, 0, kMargin), kConfident);
        EXPECT_GT(meanOfChannel(confidence, 1, kMargin), kConfident);
    }
}

TEST_F(NativeVectorGeneratorGpuTest, TvL1VectorsMatchTheMotionAndTheCpu)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kVectorGeneratorParamMethod, kVectorGeneratorMethodTVL1));
    expectVectorsMatchTruth(node, kTvL1Epe);
}

TEST_F(NativeVectorGeneratorGpuTest, RgbFlowChannelsMatchTheMotionAndTheCpu)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kVectorGeneratorParamFlowChannels, kVectorGeneratorFlowChannelsRGB));
    expectVectorsMatchTruth(node, kDisEpe);
}

TEST_F(NativeVectorGeneratorGpuTest, ColourPassesThroughBitExact)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));

    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ node }), node, windowAt(0), kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_FALSE(result.cpu.empty());
    EXPECT_EQ(result.cpu, result.gpu);
    EXPECT_TRUE(result.ranInRegion(node)) << "the node was not placed in a GPU region";
    EXPECT_TRUE(result.cpuFallbackReasons.empty()) << "the region fell back to the CPU";
    EXPECT_EQ(0, gpuRunsOf(result, node)) << "colour alone ran the solve";
}

// Colour and a solved plane of one task reach the store together, one aliased and the other downloaded.
TEST_F(NativeVectorGeneratorGpuTest, ColourBesideASolvedPlanePassesThroughBitExact)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    NodePtr reader = createReader(node, kNatronColorViewRGBA, kNatronForwardMotionVectorsLayerID);
    ASSERT_TRUE(bool(reader));

    EndToEndResult result;
    if (!renderThrough(node, reader, windowAt(0), kTime, 0, &result)) {
        return;
    }
    ASSERT_FALSE(result.cpu.empty());
    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i + 1 < result.cpu.size(); i += 4) {
        differing += (result.cpu[i] != result.gpu[i]) || (result.cpu[i + 1] != result.gpu[i + 1]);
    }
    EXPECT_EQ(0u, differing);
}

TEST_F(NativeVectorGeneratorGpuTest, FirstFrameHasNoBackwardMotion)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));

    RenderedFlow flow;
    if (!renderVectors(node, kFirstFrame, &flow)) {
        return;
    }
    EXPECT_EQ(largestMagnitude(flow.backward), 0.f);
    EXPECT_LT(meanDistance(flow.forward, kShiftX, kShiftY, kMargin), kDisEpe);

    RenderedPlane confidence;
    if (renderConfidence(node, kFirstFrame, &confidence)) {
        EXPECT_GT(meanOfChannel(confidence, 0, kMargin), kConfident);
        EXPECT_EQ(largestMagnitude(confidence, 1), 0.f);
    }
}

TEST_F(NativeVectorGeneratorGpuTest, LastFrameHasNoForwardMotion)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));

    RenderedFlow flow;
    if (!renderVectors(node, kLastFrame, &flow)) {
        return;
    }
    EXPECT_EQ(largestMagnitude(flow.forward), 0.f);
    EXPECT_LT(meanDistance(flow.backward, -kShiftX, -kShiftY, kMargin), kDisEpe);

    RenderedPlane confidence;
    if (renderConfidence(node, kLastFrame, &confidence)) {
        EXPECT_EQ(largestMagnitude(confidence, 0), 0.f);
        EXPECT_GT(meanOfChannel(confidence, 1, kMargin), kConfident);
    }
}

TEST_F(NativeVectorGeneratorGpuTest, IdenticalFramesGiveZeroVectors)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(source, kMovingTextureParamMoving, { 0. }));

    RenderedFlow flow;
    if (renderVectors(node, kTime, &flow)) {
        EXPECT_LT(meanDistance(flow.forward, 0., 0., 0), kIdenticalEpe);
        EXPECT_LT(meanDistance(flow.backward, 0., 0., 0), kIdenticalEpe);
    }
}

TEST_F(NativeVectorGeneratorGpuTest, RenderScaleHalvesVectors)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));

    RenderedFlow flow;
    if (renderVectors(node, kTime, &flow, 1)) {
        EXPECT_LT(meanDistance(flow.forward, kShiftX / 2, kShiftY / 2, kMargin), 2. * kDisEpe);
        EXPECT_LT(meanDistance(flow.backward, -kShiftX / 2, -kShiftY / 2, kMargin), 2. * kDisEpe);
    }
}

TEST_F(NativeVectorGeneratorGpuTest, ForwardOutputLeavesOutTheBackwardDirection)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kVectorGeneratorParamOutputs, kVectorGeneratorOutputsForward));
    expectDirectionMatchesTruth(node, kNatronForwardMotionVectorsLayerID, kShiftX, kShiftY);

    RenderedPlane confidence;
    if (renderConfidence(node, kTime, &confidence)) {
        EXPECT_GT(meanOfChannel(confidence, 0, kMargin), kConfident);
        EXPECT_EQ(largestMagnitude(confidence, 1), 0.f);
    }
}

TEST_F(NativeVectorGeneratorGpuTest, BackwardOutputLeavesOutTheForwardDirection)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kVectorGeneratorParamOutputs, kVectorGeneratorOutputsBackward));
    expectDirectionMatchesTruth(node, kNatronBackwardMotionVectorsLayerID, -kShiftX, -kShiftY);

    RenderedPlane confidence;
    if (renderConfidence(node, kTime, &confidence)) {
        EXPECT_EQ(largestMagnitude(confidence, 0), 0.f);
        EXPECT_GT(meanOfChannel(confidence, 1, kMargin), kConfident);
    }
}

TEST_F(NativeVectorGeneratorGpuTest, TemporalSmoothingKeepsAConstantMotion)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kVectorGeneratorParamTemporalSmoothing, { 1. }));
    expectVectorsMatchTruth(node, 2. * kDisEpe);

    RenderedPlane confidence;
    if (renderConfidence(node, kFirstFrame, &confidence)) {
        EXPECT_GT(meanOfChannel(confidence, 0, kMargin), kConfident);
        EXPECT_EQ(largestMagnitude(confidence, 1), 0.f);
    }
}

TEST_F(NativeVectorGeneratorGpuTest, MattePathKeepsTheMotion)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));
    connectNodes(source, node, kVectorGeneratorInputMatte, true);

    expectVectorsMatchTruth(node, 3. * kDisEpe);
}

TEST_F(NativeVectorGeneratorGpuTest, EmptyMatteMatchesNoMatte)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    RenderedFlow without;
    if (!renderVectors(node, kTime, &without)) {
        return;
    }

    NodePtr empty = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
    ASSERT_TRUE(bool(empty));
    ASSERT_TRUE(setKnobValues(empty, kConstantParamColor, { 0. }));
    connectNodes(empty, node, kVectorGeneratorInputMatte, true);

    RenderedFlow with;
    if (renderVectors(node, kTime, &with)) {
        EXPECT_EQ(without.forward.pixels, with.forward.pixels);
        EXPECT_EQ(without.backward.pixels, with.backward.pixels);
    }
}

TEST_F(NativeVectorGeneratorGpuTest, NonFiniteSourcePixelsGiveFiniteOutput)
{
    ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR);
    ASSERT_TRUE(bool(pair.native));
    setParitySourceNoise(pair.source, true);
    setParitySourceNonFinite(pair.source, RectI(8, 8, 24, 24));

    NodePtr reader = createReader(pair.native, kNatronForwardMotionVectorsLayerID, kNatronBackwardMotionVectorsLayerID);
    ASSERT_TRUE(bool(reader));
    EndToEndResult result;
    if (!renderThrough(pair.native, reader, paritySourceWindow(pair.source, 1., 0), 1., 0, &result)) {
        return;
    }
    for (float value : result.gpu) {
        ASSERT_TRUE(std::isfinite(value));
    }
}

// The CPU solves a frame outside the window as black; the node's GPU path refuses it, having no pixels to bind.
TEST_F(NativeVectorGeneratorGpuTest, ARegionOutsideTheWindowStaysOnTheCpu)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(source, kMovingTextureParamAwayFrame, { 4. }));

    expectStaysOnCpu(node, kTime);
}

TEST_F(NativeVectorGeneratorGpuTest, FormatChangeFailsNamingBothFrames)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(source, kMovingTextureParamFormatChangeFrame, { 4. }));

    if (!expectFrameFails(node, kTime)) {
        return;
    }
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("frame 3"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("frame 4"))) << message.toStdString();
}

TEST_F(NativeVectorGeneratorGpuTest, AFailedSourceRenderFailsInsteadOfSolvingAgainstBlack)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(source, kMovingTextureParamFailFrame, { 4. }));

    expectFrameFails(node, kTime);
}

TEST_F(NativeVectorGeneratorGpuTest, APoolBelowOneImageTakesTheCpuPath)
{
    expectFallsBackWithBudget(1);
}

// Everything the region itself holds fits, so it is the solve's own scratch that is refused.
TEST_F(NativeVectorGeneratorGpuTest, APoolTooSmallForTheSolveTakesTheCpuPath)
{
    expectFallsBackWithBudget(kImagesInTightBudget * kWidth * kHeight * 4 * sizeof(float));
}

#ifdef NATRON_HAVE_GPU
TEST_F(NativeVectorGeneratorGpuTest, AbortingADisSolveStopsItAndLeavesNothingBehind)
{
    expectAbortLeavesNothingBehind(kVectorGeneratorMethodDIS);
}

TEST_F(NativeVectorGeneratorGpuTest, AbortingATvL1SolveStopsItAndLeavesNothingBehind)
{
    expectAbortLeavesNothingBehind(kVectorGeneratorMethodTVL1);
}

TEST_F(NativeVectorGeneratorGpuTest, TwoPlanesOfARegionShareOneSolve)
{
    NodePtr generator = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(generator));
    ASSERT_TRUE(setKnobValue(generator, kVectorGeneratorParamMethod, kVectorGeneratorMethodDIS));
    NodePtr confidenceOnly = createReader(generator, kVectorGeneratorConfidenceLayerID, std::string());
    ASSERT_TRUE(bool(confidenceOnly));
    std::string error;
    const int alone = countBatchesOfFrame(std::vector<NodePtr>({ generator }), confidenceOnly, windowAt(0), kTime, &error);
    if (alone < 0) {
        ASSERT_TRUE(error.empty()) << error;

        return;
    }

    NodePtr both = createReader(generator, kVectorGeneratorConfidenceLayerID, kNatronForwardMotionVectorsLayerID);
    ASSERT_TRUE(bool(both));
    const int shared = countBatchesOfFrame(std::vector<NodePtr>({ generator }), both, windowAt(0), kTime, &error);
    ASSERT_GE(shared, 0) << error;
    EXPECT_GT(alone, kBatchesOfOutputPlane * 4);
    EXPECT_LE(shared, alone + kBatchesOfOutputPlane) << "the second plane solved again";
}
#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_EXIT
