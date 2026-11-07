/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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
#include <string>

#include <gtest/gtest.h>

#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/VectorFollowTestSupport.h"

#include "Engine/Node.h"
#include "Engine/Nodes/Time/VectorGenerator.h"
#include "Engine/VectorSampler.h"

NATRON_NAMESPACE_ENTER

namespace {

using namespace VectorFollowTest;

constexpr double kTolerance = 0.1;

class VectorSamplerTest
    : public NativeTimeNodeTest {
public:
    void SetUp() override
    {
        NativeTimeNodeTest::SetUp();
        registerFollowTextureOnce();
    }

    NodePtr createTexture()
    {
        return createNode(QString::fromUtf8(kTestPluginIDFollowTexture), 1);
    }

    NodePtr createGenerator(const NodePtr& input)
    {
        return createOver(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, input);
    }
};

TEST_F(VectorSamplerTest, ReadsTheVectorsOfATranslatingPicture)
{
    NodePtr generator = createGenerator(createTexture());
    ASSERT_TRUE(bool(generator));

    VectorSampler sampler(generator);
    ASSERT_TRUE(sampler.prepareFrame(10)) << sampler.lastError();

    const VectorSample s = sampler.sample(10, kWidth / 2.25, kHeight / 2.25);
    EXPECT_NEAR(s.forwardX, kShiftX, kTolerance);
    EXPECT_NEAR(s.forwardY, kShiftY, kTolerance);
    EXPECT_NEAR(s.backwardX, -kShiftX, kTolerance);
    EXPECT_NEAR(s.backwardY, -kShiftY, kTolerance);
    EXPECT_GT(s.forwardConfidence, kVectorFollowDefaultConfidenceThreshold);
    EXPECT_GT(s.backwardConfidence, kVectorFollowDefaultConfidenceThreshold);
}

TEST_F(VectorSamplerTest, OutsideTheRegionClampsWithNoConfidence)
{
    NodePtr generator = createGenerator(createTexture());
    ASSERT_TRUE(bool(generator));

    VectorSampler sampler(generator);
    ASSERT_TRUE(sampler.prepareFrame(10)) << sampler.lastError();

    const VectorSample border = sampler.sample(10, 0.5, kHeight / 2.);
    const VectorSample outside = sampler.sample(10, -500., kHeight / 2.);
    EXPECT_DOUBLE_EQ(outside.forwardX, border.forwardX);
    EXPECT_DOUBLE_EQ(outside.forwardY, border.forwardY);
    EXPECT_EQ(outside.forwardConfidence, 0.);
    EXPECT_EQ(outside.backwardConfidence, 0.);
}

TEST_F(VectorSamplerTest, AnAbortedSamplerReadsNothing)
{
    NodePtr generator = createGenerator(createTexture());
    ASSERT_TRUE(bool(generator));

    VectorSampler sampler(generator);
    sampler.abort();
    EXPECT_TRUE(sampler.isAborted());
    EXPECT_FALSE(sampler.prepareFrame(10));
    EXPECT_EQ(sampler.sample(10, 10., 10.).forwardConfidence, 0.);
}

TEST_F(VectorSamplerTest, KeepsFewFramesButCanReadAnyAgain)
{
    NodePtr generator = createGenerator(createTexture());
    ASSERT_TRUE(bool(generator));

    VectorSampler sampler(generator, 1);
    ASSERT_TRUE(sampler.prepareFrame(10)) << sampler.lastError();
    ASSERT_TRUE(sampler.prepareFrame(11)) << sampler.lastError();
    EXPECT_NEAR(sampler.sample(10, kWidth / 2.25, kHeight / 2.25).forwardX, kShiftX, kTolerance);
}

TEST_F(VectorSamplerTest, HasMotionLayersOnlyWhenTheNodeOutputsThem)
{
    NodePtr texture = createTexture();
    NodePtr generator = createGenerator(texture);
    ASSERT_TRUE(bool(generator));

    EXPECT_FALSE(VectorSampler::hasMotionLayers(texture, 10.));
    EXPECT_TRUE(VectorSampler::hasMotionLayers(generator, 10.));
    EXPECT_FALSE(VectorSampler::hasMotionLayers(NodePtr(), 10.));
}

TEST_F(VectorSamplerTest, AForwardOnlySourceIsListedForForwardFollowsAlone)
{
    NodePtr generator = createGenerator(createTexture());
    ASSERT_TRUE(bool(generator));
    setKnob<KnobChoice>(generator, kVectorGeneratorParamOutputs, (int)eVectorGeneratorOutputsForward);
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));

    EXPECT_FALSE(VectorSampler::hasMotionLayers(generator, 10.));
    EXPECT_TRUE(VectorSampler::hasForwardMotionLayer(generator, 10.));

    std::string error;
    EXPECT_TRUE(VectorSampler::checkSource(generator, roto, 10., &error)) << error;
    EXPECT_TRUE(VectorSampler::checkSource(generator, roto, 10., VectorFollowDirection::eForward, &error)) << error;
    EXPECT_FALSE(VectorSampler::checkSource(generator, roto, 10., VectorFollowDirection::eBackward, &error));
    EXPECT_NE(error.find("no backward motion layer"), std::string::npos) << error;
}

TEST_F(VectorSamplerTest, AForwardOnlySourceReadsWithNoBackwardConfidence)
{
    NodePtr generator = createGenerator(createTexture());
    ASSERT_TRUE(bool(generator));
    setKnob<KnobChoice>(generator, kVectorGeneratorParamOutputs, (int)eVectorGeneratorOutputsForward);

    VectorSampler sampler(generator);
    ASSERT_TRUE(sampler.prepareFrame(10)) << sampler.lastError();

    const VectorSample s = sampler.sample(10, kWidth / 2.25, kHeight / 2.25);
    EXPECT_NEAR(s.forwardX, kShiftX, kTolerance);
    EXPECT_NEAR(s.forwardY, kShiftY, kTolerance);
    EXPECT_GT(s.forwardConfidence, kVectorFollowDefaultConfidenceThreshold);
    EXPECT_EQ(s.backwardX, 0.);
    EXPECT_EQ(s.backwardY, 0.);
    EXPECT_EQ(s.backwardConfidence, 0.);
}

TEST_F(VectorSamplerTest, RefusesASourceWithoutLayers)
{
    NodePtr texture = createTexture();
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));

    std::string error;
    EXPECT_FALSE(VectorSampler::checkSource(texture, roto, 10., &error));
    EXPECT_NE(error.find(texture->getScriptName()), std::string::npos);
}

TEST_F(VectorSamplerTest, RefusesTheHostAndWhatIsDownstreamOfIt)
{
    NodePtr texture = createTexture();
    NodePtr roto = createOver(PLUGINID_NATRON_ROTO, 1, texture);
    NodePtr downstream = createGenerator(roto);
    ASSERT_TRUE(bool(roto));
    ASSERT_TRUE(bool(downstream));

    std::string error;
    EXPECT_FALSE(VectorSampler::checkSource(roto, roto, 10., &error));
    EXPECT_FALSE(VectorSampler::checkSource(downstream, roto, 10., &error));
    EXPECT_NE(error.find(roto->getScriptName()), std::string::npos);
}

TEST_F(VectorSamplerTest, AcceptsAGeneratorUpstreamOfOrBesideTheHost)
{
    NodePtr texture = createTexture();
    NodePtr beside = createGenerator(texture);
    NodePtr roto = createOver(PLUGINID_NATRON_ROTO, 1, beside);
    ASSERT_TRUE(bool(roto));

    std::string error;
    EXPECT_TRUE(VectorSampler::checkSource(beside, roto, 10., &error)) << error;
}

} // anonymous namespace

NATRON_NAMESPACE_EXIT
