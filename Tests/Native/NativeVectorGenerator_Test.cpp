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
#include <cmath>
#include <limits>
#include <list>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativePluginIdentity.h"
#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/Native/NativeVectorGeneratorTest.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Time/VectorGenerator.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

using namespace NativeVectorGeneratorPlate;

class NativeVectorGeneratorTest
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

    // The node's plane; a failed render is a test failure and an empty plane.
    RenderedPlane plane(const NodePtr& node,
                        double time,
                        const ImageLayerDesc& layer,
                        unsigned mipmapLevel = 0)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool ok = renderNodePlanesDirect(node, time, ViewIdx(0), mipmapLevel, windowAt(mipmapLevel),
                                               std::list<ImageLayerDesc> { layer }, &planes, &error);
        EXPECT_TRUE(ok) << error;

        return planes.empty() ? RenderedPlane() : planes[0];
    }

    std::vector<double> framesNeeded(const NodePtr& node,
                                     double time,
                                     int inputNb)
    {
        const EffectInstancePtr effect = node->getEffectInstance();
        const FramesNeededMap needed = effect->getFramesNeeded_public(effect->getRenderHash(), time, ViewIdx(0), 0);
        std::vector<double> frames;
        const FramesNeededMap::const_iterator input = needed.find(inputNb);
        if (input == needed.end()) {
            return frames;
        }
        const FrameRangesMap::const_iterator ranges = input->second.find(ViewIdx(0));
        if (ranges == input->second.end()) {
            return frames;
        }
        for (const RangeD& range : ranges->second) {
            for (double t = range.min; t <= range.max; t += 1.) {
                frames.push_back(t);
            }
        }
        std::sort(frames.begin(), frames.end());

        return frames;
    }

    bool outputHasLayer(const NodePtr& node,
                        const ImageLayerDesc& layer)
    {
        std::list<ImageLayerDesc> layers;
        // getAvailableLayers() adds every project-registered layer, the motion ones included.
        node->getEffectInstance()->getPresentLayers(3., ViewIdx(0), -1, &layers);
        for (const ImageLayerDesc& present : layers) {
            if (present.getLayerID() == layer.getLayerID()) {
                return true;
            }
        }

        return false;
    }

    void expectVectorsMatchTruth(const NodePtr& node,
                                 double threshold)
    {
        EXPECT_LT(meanDistance(plane(node, 3., forwardLayer()), kShiftX, kShiftY, kMargin), threshold);
        EXPECT_LT(meanDistance(plane(node, 3., backwardLayer()), -kShiftX, -kShiftY, kMargin), threshold);
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, ParityTolerance::make(1e-3, 0.), true);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 4) << caseName;
        }
    }
};

TEST(NativeVectorGeneratorCurve, MatchingValueIsMonotonicAndBoundedForEveryInput)
{
    EXPECT_FLOAT_EQ(vectorGeneratorMatchingValue(0.f), 0.f);
    EXPECT_FLOAT_EQ(vectorGeneratorMatchingValue(1.f), 1.f);
    EXPECT_EQ(vectorGeneratorMatchingValue(-0.5f), 0.f);
    EXPECT_EQ(vectorGeneratorMatchingValue(-std::numeric_limits<float>::infinity()), 0.f);
    EXPECT_EQ(vectorGeneratorMatchingValue(std::numeric_limits<float>::infinity()), 0.f);
    EXPECT_EQ(vectorGeneratorMatchingValue(std::numeric_limits<float>::quiet_NaN()), 0.f);

    float previous = 0.f;
    for (float v = 1e-4f; v < 1e6f; v *= 2.f) {
        const float matched = vectorGeneratorMatchingValue(v);
        EXPECT_GT(matched, previous) << v;
        EXPECT_TRUE(std::isfinite(matched)) << v;
        previous = matched;
    }
    EXPECT_TRUE(std::isfinite(vectorGeneratorMatchingValue(std::numeric_limits<float>::max())));

    // Equal ratios stay close to equal steps: a highlight does not swamp the shadows' gradients.
    const float stops[3] = { 1.f, 2.f, 4.f };
    const float firstStop = vectorGeneratorMatchingValue(stops[1]) - vectorGeneratorMatchingValue(stops[0]);
    const float secondStop = vectorGeneratorMatchingValue(stops[2]) - vectorGeneratorMatchingValue(stops[1]);
    EXPECT_NEAR(firstStop, secondStop, 0.05f * firstStop);
}

TEST_F(NativeVectorGeneratorTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, "VectorGenerator");
}

TEST_F(NativeVectorGeneratorTest, DisVectorsMatchTheMotion)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kVectorGeneratorParamMethod, kVectorGeneratorMethodDIS));
    expectVectorsMatchTruth(node, kDisEpe);

    const RenderedPlane confidence = plane(node, 3., VectorGenerator::confidenceLayer());
    EXPECT_GT(meanOfChannel(confidence, 0, kMargin), kConfident);
    EXPECT_GT(meanOfChannel(confidence, 1, kMargin), kConfident);
}

TEST_F(NativeVectorGeneratorTest, TvL1VectorsMatchTheMotion)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kVectorGeneratorParamMethod, kVectorGeneratorMethodTVL1));
    expectVectorsMatchTruth(node, kTvL1Epe);
}

TEST_F(NativeVectorGeneratorTest, RgbFlowChannelsMatchTheMotion)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kVectorGeneratorParamFlowChannels, kVectorGeneratorFlowChannelsRGB));
    expectVectorsMatchTruth(node, kDisEpe);
}

TEST_F(NativeVectorGeneratorTest, ColourPassesThroughBitExact)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));

    const RenderedPlane expected = plane(source, 3., ImageLayerDesc::getRGBAComponents());
    const RenderedPlane actual = plane(node, 3., ImageLayerDesc::getRGBAComponents());
    ASSERT_FALSE(expected.pixels.empty());
    EXPECT_EQ(expected.pixels, actual.pixels);
}

TEST_F(NativeVectorGeneratorTest, RegionOfDefinitionIsTheSources)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));
    const EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    ASSERT_EQ(effect->getRegionOfDefinition_public(effect->getRenderHash(), 3., RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat), eStatusOK);
    EXPECT_EQ(rod.x1, 0.);
    EXPECT_EQ(rod.y1, 0.);
    EXPECT_EQ(rod.x2, (double)kWidth);
    EXPECT_EQ(rod.y2, (double)kHeight);
}

TEST_F(NativeVectorGeneratorTest, FirstFrameHasNoBackwardMotion)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));

    EXPECT_EQ(largestMagnitude(plane(node, kFirstFrame, backwardLayer())), 0.f);
    EXPECT_LT(meanDistance(plane(node, kFirstFrame, forwardLayer()), kShiftX, kShiftY, kMargin), kDisEpe);

    const RenderedPlane confidence = plane(node, kFirstFrame, VectorGenerator::confidenceLayer());
    EXPECT_GT(meanOfChannel(confidence, 0, kMargin), kConfident);
    EXPECT_EQ(largestMagnitude(confidence, 1), 0.f);
}

TEST_F(NativeVectorGeneratorTest, LastFrameHasNoForwardMotion)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));

    EXPECT_EQ(largestMagnitude(plane(node, kLastFrame, forwardLayer())), 0.f);
    EXPECT_LT(meanDistance(plane(node, kLastFrame, backwardLayer()), -kShiftX, -kShiftY, kMargin), kDisEpe);

    const RenderedPlane confidence = plane(node, kLastFrame, VectorGenerator::confidenceLayer());
    EXPECT_EQ(largestMagnitude(confidence, 0), 0.f);
    EXPECT_GT(meanOfChannel(confidence, 1, kMargin), kConfident);
}

TEST_F(NativeVectorGeneratorTest, IdenticalFramesGiveZeroVectors)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(source, kMovingTextureParamMoving, { 0. }));

    EXPECT_LT(meanDistance(plane(node, 3., forwardLayer()), 0., 0., 0), kIdenticalEpe);
    EXPECT_LT(meanDistance(plane(node, 3., backwardLayer()), 0., 0., 0), kIdenticalEpe);
}

TEST_F(NativeVectorGeneratorTest, RenderScaleHalvesVectors)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));

    const RenderedPlane forward = plane(node, 3., forwardLayer(), 1);
    EXPECT_EQ(forward.window.width(), kWidth / 2);
    EXPECT_LT(meanDistance(forward, kShiftX / 2, kShiftY / 2, kMargin), 2. * kDisEpe);
    EXPECT_LT(meanDistance(plane(node, 3., backwardLayer(), 1), -kShiftX / 2, -kShiftY / 2, kMargin), 2. * kDisEpe);
}

TEST_F(NativeVectorGeneratorTest, FormatChangeFailsNamingBothFrames)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(source, kMovingTextureParamFormatChangeFrame, { 4. }));

    std::vector<RenderedPlane> planes;
    EXPECT_FALSE(renderNodePlanesDirect(node, 3., ViewIdx(0), 0, windowAt(0), std::list<ImageLayerDesc> { forwardLayer() }, &planes));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("frame 3"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("frame 4"))) << message.toStdString();

    // Frames 1 and 2 share a format, and the error goes with the render that follows.
    EXPECT_LT(meanDistance(plane(node, kFirstFrame, forwardLayer()), kShiftX, kShiftY, kMargin), kDisEpe);
    EXPECT_FALSE(node->hasPersistentMessage());
}

TEST_F(NativeVectorGeneratorTest, FormatOriginChangeFailsNamingBothFrames)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(source, kMovingTextureParamOriginChangeFrame, { 4. }));

    std::vector<RenderedPlane> planes;
    EXPECT_FALSE(renderNodePlanesDirect(node, 3., ViewIdx(0), 0, windowAt(0), std::list<ImageLayerDesc> { forwardLayer() }, &planes));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("frame 3"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("frame 4"))) << message.toStdString();
}

TEST_F(NativeVectorGeneratorTest, ARegionOutsideTheWindowSolvesAsABlackFrame)
{
    for (const double smoothing : { 0., 1. }) {
        NodePtr away = createMovingTexture();
        NodePtr black = createMovingTexture();
        NodePtr awayNode = createGenerator(away);
        NodePtr blackNode = createGenerator(black);
        ASSERT_TRUE(bool(awayNode));
        ASSERT_TRUE(bool(blackNode));
        ASSERT_TRUE(setKnobValues(away, kMovingTextureParamAwayFrame, { 4. }));
        ASSERT_TRUE(setKnobValues(black, kMovingTextureParamBlackFrame, { 4. }));
        ASSERT_TRUE(setKnobValues(awayNode, kVectorGeneratorParamTemporalSmoothing, { smoothing }));
        ASSERT_TRUE(setKnobValues(blackNode, kVectorGeneratorParamTemporalSmoothing, { smoothing }));

        const RenderedPlane awayForward = plane(awayNode, 3., forwardLayer());
        const RenderedPlane awayConfidence = plane(awayNode, 3., VectorGenerator::confidenceLayer());
        EXPECT_FALSE(awayNode->hasPersistentMessage());
        ASSERT_FALSE(awayForward.pixels.empty());
        EXPECT_EQ(awayForward.pixels, plane(blackNode, 3., forwardLayer()).pixels);
        EXPECT_EQ(awayConfidence.pixels, plane(blackNode, 3., VectorGenerator::confidenceLayer()).pixels);
        // A frame the sequence does not have leaves its direction at confidence 0 instead.
        EXPECT_GT(largestMagnitude(awayConfidence, 0), 0.f);
    }
}

TEST_F(NativeVectorGeneratorTest, AFailedSourceRenderFailsInsteadOfSolvingAgainstBlack)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));

    // The same render succeeds while the failing frame is one it does not need, so the failure below is the source's.
    ASSERT_TRUE(setKnobValues(source, kMovingTextureParamFailFrame, { 100. }));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderNodePlanesDirect(node, 3., ViewIdx(0), 0, windowAt(0), std::list<ImageLayerDesc> { forwardLayer() }, &planes));

    ASSERT_TRUE(setKnobValues(source, kMovingTextureParamFailFrame, { 4. }));
    // The host fails a render whose input render failed before this node's render runs, so the node
    // posts no message of its own for it; the render result is what must not be a solve against black.
    EXPECT_FALSE(renderNodePlanesDirect(node, 3., ViewIdx(0), 0, windowAt(0), std::list<ImageLayerDesc> { forwardLayer() }, &planes, nullptr, true /*byPassCache*/));
}

TEST_F(NativeVectorGeneratorTest, ForwardOutputLeavesOutTheBackwardLayerAndItsFrames)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(outputHasLayer(node, backwardLayer()));

    ASSERT_TRUE(setKnobValue(node, kVectorGeneratorParamOutputs, kVectorGeneratorOutputsForward));
    EXPECT_TRUE(outputHasLayer(node, forwardLayer()));
    EXPECT_FALSE(outputHasLayer(node, backwardLayer()));
    EXPECT_TRUE(outputHasLayer(node, VectorGenerator::confidenceLayer()));
    EXPECT_EQ(framesNeeded(node, 3., kVectorGeneratorInputSource), (std::vector<double> { 3., 4. }));

    const RenderedPlane confidence = plane(node, 3., VectorGenerator::confidenceLayer());
    EXPECT_GT(meanOfChannel(confidence, 0, kMargin), kConfident);
    EXPECT_EQ(largestMagnitude(confidence, 1), 0.f);
}

TEST_F(NativeVectorGeneratorTest, BackwardOutputLeavesOutTheForwardLayerAndItsFrames)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));

    ASSERT_TRUE(setKnobValue(node, kVectorGeneratorParamOutputs, kVectorGeneratorOutputsBackward));
    EXPECT_FALSE(outputHasLayer(node, forwardLayer()));
    EXPECT_TRUE(outputHasLayer(node, backwardLayer()));
    EXPECT_EQ(framesNeeded(node, 3., kVectorGeneratorInputSource), (std::vector<double> { 2., 3. }));

    const RenderedPlane confidence = plane(node, 3., VectorGenerator::confidenceLayer());
    EXPECT_EQ(largestMagnitude(confidence, 0), 0.f);
    EXPECT_GT(meanOfChannel(confidence, 1, kMargin), kConfident);
}

TEST_F(NativeVectorGeneratorTest, FramesNeededStayInTheSourcesDomain)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));

    EXPECT_EQ(framesNeeded(node, 3., kVectorGeneratorInputSource), (std::vector<double> { 2., 3., 4. }));
    EXPECT_EQ(framesNeeded(node, kFirstFrame, kVectorGeneratorInputSource), (std::vector<double> { 1., 2. }));
    EXPECT_EQ(framesNeeded(node, kLastFrame, kVectorGeneratorInputSource), (std::vector<double> { 4., 5. }));
    EXPECT_TRUE(framesNeeded(node, 3., kVectorGeneratorInputMatte).empty());

    ASSERT_TRUE(setKnobValues(node, kVectorGeneratorParamTemporalSmoothing, { 1. }));
    EXPECT_EQ(framesNeeded(node, 3., kVectorGeneratorInputSource), (std::vector<double> { 1., 2., 3., 4., 5. }));
    EXPECT_EQ(framesNeeded(node, kFirstFrame, kVectorGeneratorInputSource), (std::vector<double> { 1., 2., 3. }));
    EXPECT_EQ(framesNeeded(node, kLastFrame, kVectorGeneratorInputSource), (std::vector<double> { 3., 4., 5. }));
}

TEST_F(NativeVectorGeneratorTest, TemporalSmoothingKeepsAConstantMotion)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kVectorGeneratorParamTemporalSmoothing, { 1. }));
    expectVectorsMatchTruth(node, 2. * kDisEpe);

    const RenderedPlane confidence = plane(node, kFirstFrame, VectorGenerator::confidenceLayer());
    EXPECT_GT(meanOfChannel(confidence, 0, kMargin), kConfident);
    EXPECT_EQ(largestMagnitude(confidence, 1), 0.f);
}

TEST_F(NativeVectorGeneratorTest, MattePathKeepsTheMotion)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));
    connectNodes(source, node, kVectorGeneratorInputMatte, true);

    EXPECT_EQ(framesNeeded(node, 3., kVectorGeneratorInputMatte), (std::vector<double> { 2., 3., 4. }));
    expectVectorsMatchTruth(node, 3. * kDisEpe);
}

TEST_F(NativeVectorGeneratorTest, EmptyMatteMatchesNoMatte)
{
    NodePtr source = createMovingTexture();
    NodePtr node = createGenerator(source);
    ASSERT_TRUE(bool(node));
    const RenderedPlane without = plane(node, 3., forwardLayer());

    NodePtr empty = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
    ASSERT_TRUE(bool(empty));
    ASSERT_TRUE(setKnobValues(empty, kConstantParamColor, { 0. }));
    connectNodes(empty, node, kVectorGeneratorInputMatte, true);

    const RenderedPlane with = plane(node, 3., forwardLayer());
    ASSERT_FALSE(without.pixels.empty());
    EXPECT_EQ(without.pixels, with.pixels);
}

TEST_F(NativeVectorGeneratorTest, SchedulerModesAgree)
{
    NodePtr node = createGenerator(createMovingTexture());
    ASSERT_TRUE(bool(node));
    const std::list<ImageLayerDesc> layers { forwardLayer(), backwardLayer(), VectorGenerator::confidenceLayer() };

    std::vector<RenderedPlane> legacy;
    std::vector<RenderedPlane> taskGraph;
    std::string error;
    ASSERT_TRUE(renderNodePlanesInMode(node, 3., ViewIdx(0), 0, windowAt(0), layers, eRenderSchedulerModeLegacy, true, &legacy, &error)) << error;
    ASSERT_TRUE(renderNodePlanesInMode(node, 3., ViewIdx(0), 0, windowAt(0), layers, eRenderSchedulerModeTaskGraph, true, &taskGraph, &error)) << error;
    ASSERT_EQ(legacy.size(), layers.size());
    ASSERT_EQ(taskGraph.size(), layers.size());
    // The task graph returns its planes in layer order, Legacy in the order of `layers`.
    for (const RenderedPlane& expected : legacy) {
        const std::vector<RenderedPlane>::const_iterator found = std::find_if(taskGraph.begin(), taskGraph.end(), [&](const RenderedPlane& rendered) {
            return rendered.layer.getLayerID() == expected.layer.getLayerID();
        });
        ASSERT_NE(found, taskGraph.end()) << expected.layer.getLayerID();
        EXPECT_EQ(expected.pixels, found->pixels) << expected.layer.getLayerID();
    }
}

TEST_F(NativeVectorGeneratorTest, NonFiniteSourcePixelsGiveFiniteOutput)
{
    ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR);
    ASSERT_TRUE(bool(pair.native));
    setParitySourceNoise(pair.source, true);
    setParitySourceNonFinite(pair.source, RectI(8, 8, 24, 24));

    const RectI window = paritySourceWindow(pair.source, 1., 0);
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(pair.native, 1., ViewIdx(0), 0, window,
                                       std::list<ImageLayerDesc> { forwardLayer(), backwardLayer(), VectorGenerator::confidenceLayer() },
                                       &planes, &error))
        << error;
    for (const RenderedPlane& rendered : planes) {
        for (float value : rendered.pixels) {
            ASSERT_TRUE(std::isfinite(value)) << rendered.layer.getLayerID();
        }
    }
}

TEST_F(NativeVectorGeneratorTest, RegressionDis)
{
    ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR);
    ASSERT_TRUE(bool(pair.native));
    setParitySourceNoise(pair.source, true);
    expectParity(pair, "dis");
}

TEST_F(NativeVectorGeneratorTest, RegressionTvL1)
{
    ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR);
    ASSERT_TRUE(bool(pair.native));
    setParitySourceNoise(pair.source, true);
    ASSERT_TRUE(setKnobValue(pair.native, kVectorGeneratorParamMethod, kVectorGeneratorMethodTVL1));
    expectParity(pair, "tvl1");
}

TEST_F(NativeVectorGeneratorTest, RegressionSmoothing)
{
    ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR);
    ASSERT_TRUE(bool(pair.native));
    setParitySourceNoise(pair.source, true);
    ASSERT_TRUE(setKnobValues(pair.native, kVectorGeneratorParamTemporalSmoothing, { 1. }));
    expectParity(pair, "smoothing");
}

NATRON_NAMESPACE_EXIT
