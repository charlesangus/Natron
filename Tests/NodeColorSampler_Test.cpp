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

#include <array>
#include <atomic>
#include <cmath>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QString>
#include <QThread>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "NativeParity.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kSize = 64;
// Far longer than the abort may take, so that only an abort ends the render in time.
const qint64 kSlowRenderMs = 20000;
const qint64 kPromptAbortMs = 2000;

} // namespace

class NodeColorSamplerTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kSize, kSize, "nodeColorSampler64", 1.));
        appPTR->clearAllCaches();
    }

    virtual void TearDown() OVERRIDE
    {
        appPTR->clearAllCaches();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr makeNode(const char* pluginID,
                     int major)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID), major);
        EXPECT_TRUE(bool(node)) << pluginID;

        return node;
    }

    static bool sample(const NodePtr& node,
                       const RectI& rect,
                       float rgba[4])
    {
        return sampleNodeColor(node, kTime, ViewIdx(0), 0, rect, ImageLayerDesc::getRGBAComponents(), rgba);
    }

    // A Constant of colour rgba over box only, so the region of definition ends at box.
    NodePtr makeBoxConstant(const RectD& box,
                            const std::vector<double>& rgba)
    {
        NodePtr node = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
        KnobChoice* extent = node ? dynamic_cast<KnobChoice*>(node->getKnobByName(kNativeGeneratorParamExtent).get()) : NULL;
        EXPECT_TRUE(extent != NULL);
        if (!extent) {
            return NodePtr();
        }
        const std::vector<ChoiceOption> entries = extent->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == kNativeGeneratorExtentSize) {
                extent->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
            }
        }
        if (!setKnobValues(node, kNativeGeneratorParamBottomLeft, { box.x1, box.y1 }) || !setKnobValues(node, kNativeGeneratorParamSize, { box.width(), box.height() }) || !setKnobValues(node, kConstantParamColor, rgba)) {
            return NodePtr();
        }

        return node;
    }
};

TEST_F(NodeColorSamplerTest, SamplesANodeAndItsInputSeparately)
{
    NodePtr constant = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr grade = makeNode(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(bool(constant) && bool(grade));
    ASSERT_TRUE(setKnobValues(constant, kConstantParamColor, { 0.25 }));
    ASSERT_TRUE(setKnobValues(grade, kGradeParamMultiply, { 2. }));
    connectNodes(constant, grade, 0, true);

    const RectI pixel(10, 10, 11, 11);
    float graded[4] = { -1.f, -1.f, -1.f, -1.f };
    ASSERT_TRUE(sample(grade, pixel, graded));
    float source[4] = { -1.f, -1.f, -1.f, -1.f };
    ASSERT_TRUE(sample(grade->getInput(0), pixel, source));
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(0.5f, graded[c], 1e-5f) << "channel " << c;
        EXPECT_NEAR(0.25f, source[c], 1e-5f) << "channel " << c;
    }

    float area[4] = { -1.f, -1.f, -1.f, -1.f };
    ASSERT_TRUE(sample(grade, RectI(4, 4, 20, 12), area));
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(0.5f, area[c], 1e-5f) << "channel " << c;
    }
}

// Boxes of 8 pixels with no lines meet at the centre (32, 32); in the rows just below it the
// boxes left of x = 32 use color2 and those right of it color3.
TEST_F(NodeColorSamplerTest, AveragesARectAcrossABoxBoundary)
{
    NodePtr checker = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    ASSERT_TRUE(bool(checker));
    ASSERT_TRUE(setKnobValues(checker, kCheckerBoardParamBoxSize, { 8. }));
    ASSERT_TRUE(setKnobValues(checker, kCheckerBoardParamLineWidth, { 0. }));
    ASSERT_TRUE(setKnobValues(checker, kCheckerBoardParamCenterLineWidth, { 0. }));
    ASSERT_TRUE(setKnobValues(checker, kCheckerBoardParamColor0, { 0. }));
    ASSERT_TRUE(setKnobValues(checker, kCheckerBoardParamColor1, { 1. }));
    ASSERT_TRUE(setKnobValues(checker, kCheckerBoardParamColor2, { 0. }));
    ASSERT_TRUE(setKnobValues(checker, kCheckerBoardParamColor3, { 1. }));

    const RectI rect(29, 28, 33, 32);
    double sums[4] = { 0., 0., 0., 0. };
    bool sawZero = false;
    bool sawOne = false;
    for (int y = rect.y1; y < rect.y2; ++y) {
        for (int x = rect.x1; x < rect.x2; ++x) {
            float pixel[4];
            ASSERT_TRUE(sample(checker, RectI(x, y, x + 1, y + 1), pixel)) << x << "," << y;
            for (int c = 0; c < 4; ++c) {
                sums[c] += pixel[c];
            }
            sawZero = sawZero || (pixel[0] == 0.f);
            sawOne = sawOne || (pixel[0] == 1.f);
        }
    }
    EXPECT_TRUE(sawZero && sawOne) << "the rect does not cross a box boundary";

    float mean[4] = { -1.f, -1.f, -1.f, -1.f };
    ASSERT_TRUE(sample(checker, rect, mean));
    const double area = rect.width() * rect.height();
    for (int c = 0; c < 4; ++c) {
        EXPECT_NEAR(sums[c] / area, mean[c], 1e-5) << "channel " << c;
    }
    EXPECT_NEAR(0.25f, mean[0], 1e-5f);
}

TEST_F(NodeColorSamplerTest, DisconnectedNodeReturnsFalse)
{
    NodePtr grade = makeNode(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(bool(grade));

    float rgba[4] = { -1.f, -1.f, -1.f, -1.f };
    EXPECT_FALSE(sample(grade, RectI(10, 10, 11, 11), rgba));
    EXPECT_FALSE(sample(NodePtr(), RectI(10, 10, 11, 11), rgba));
    for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(-1.f, rgba[c]) << "channel " << c;
    }
}

// The Object Select tool renders its Source this way on a thread of its own, and aborts it when the
// project closes or the node goes: the call must return soon after, not when the render is done.
TEST_F(NodeColorSamplerTest, AnAbortedRenderReturnsPromptly)
{
    CountingTestRegistry::reset();
    NodePtr slow = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(slow));

    std::atomic<bool> rendering(false);
    CountingTestRegistry::setRenderHook([&rendering](const Node* node) {
        rendering = true;
        QElapsedTimer timer;
        timer.start();
        while (!node->aborted() && (timer.elapsed() < kSlowRenderMs)) {
            QThread::msleep(5);
        }
    });

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
    ImagePtr image;
    std::thread worker([&slow, &abortInfo, &image]() {
        image = renderNodeImage(slow, kTime, ViewIdx(0), 0, RectI(0, 0, kCountingTestSize, kCountingTestSize),
                                ImageLayerDesc::getRGBAComponents(), NULL, abortInfo);
    });

    QElapsedTimer started;
    started.start();
    while (!rendering && (started.elapsed() < kSlowRenderMs)) {
        QThread::msleep(5);
    }
    EXPECT_TRUE(rendering.load()) << "the render never started";

    QElapsedTimer aborting;
    aborting.start();
    abortInfo->setAborted();
    worker.join();
    EXPECT_LT(aborting.elapsed(), kPromptAbortMs);
    EXPECT_FALSE(bool(image)) << "an aborted render hands back no image";

    CountingTestRegistry::reset();
}

TEST_F(NodeColorSamplerTest, SampleInputAtReadsTheInputNotTheNode)
{
    NodePtr constant = makeBoxConstant(RectD(8., 8., 24., 24.), { 0.25, 0.5, 0.75, 1. });
    NodePtr grade = makeNode(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(bool(constant) && bool(grade));
    ASSERT_TRUE(setKnobValues(grade, kGradeParamMultiply, { 2. }));
    connectNodes(constant, grade, 0, true);

    const std::optional<std::array<float, 4>> rgba = sampleNodeInputAt(grade, 0, kTime, ViewIdx(0), 10.5, 20.9);
    ASSERT_TRUE(rgba.has_value());
    const float expected[4] = { 0.25f, 0.5f, 0.75f, 1.f };
    for (int c = 0; c < 4; ++c) {
        EXPECT_NEAR(expected[c], (*rgba)[c], 1e-5f) << "channel " << c;
    }
}

TEST_F(NodeColorSamplerTest, SampleInputAtOutsideTheRegionOfDefinitionIsNone)
{
    NodePtr constant = makeBoxConstant(RectD(8., 8., 24., 24.), { 0.25, 0.5, 0.75, 1. });
    NodePtr grade = makeNode(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(bool(constant) && bool(grade));
    connectNodes(constant, grade, 0, true);

    EXPECT_TRUE(sampleNodeInputAt(grade, 0, kTime, ViewIdx(0), 23.5, 23.5).has_value());
    EXPECT_FALSE(sampleNodeInputAt(grade, 0, kTime, ViewIdx(0), 24.5, 12.).has_value());
    EXPECT_FALSE(sampleNodeInputAt(grade, 0, kTime, ViewIdx(0), 12., 7.5).has_value());
    EXPECT_FALSE(sampleNodeInputAt(grade, 0, kTime, ViewIdx(0), 40., 40.).has_value());
    EXPECT_FALSE(sampleNodeInputAt(grade, 0, kTime, ViewIdx(0), -1e300, 12.).has_value());
    EXPECT_FALSE(sampleNodeInputAt(grade, 0, kTime, ViewIdx(0), std::nan(""), 12.).has_value());
}

TEST_F(NodeColorSamplerTest, SampleInputAtMatchesTheInputPixelByPixel)
{
    NodePtr checker = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeNode(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(bool(checker) && bool(grade));
    ASSERT_TRUE(setKnobValues(checker, kCheckerBoardParamBoxSize, { 8. }));
    ASSERT_TRUE(setKnobValues(grade, kGradeParamMultiply, { 3. }));
    connectNodes(checker, grade, 0, true);

    for (int y = 26; y < 38; y += 3) {
        for (int x = 26; x < 38; x += 3) {
            float pixel[4];
            ASSERT_TRUE(sample(checker, RectI(x, y, x + 1, y + 1), pixel)) << x << "," << y;
            const std::optional<std::array<float, 4>> rgba = sampleNodeInputAt(grade, 0, kTime, ViewIdx(0), x + 0.75, y + 0.25);
            ASSERT_TRUE(rgba.has_value()) << x << "," << y;
            for (int c = 0; c < 4; ++c) {
                EXPECT_EQ(pixel[c], (*rgba)[c]) << x << "," << y << " channel " << c;
            }
        }
    }
}

TEST_F(NodeColorSamplerTest, SampleInputAtLooksThroughADisabledInput)
{
    NodePtr constant = makeBoxConstant(RectD(0., 0., 32., 32.), { 0.25, 0.25, 0.25, 1. });
    NodePtr disabled = makeNode(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    NodePtr grade = makeNode(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(bool(constant) && bool(disabled) && bool(grade));
    ASSERT_TRUE(setKnobValues(disabled, kGradeParamMultiply, { 2. }));
    connectNodes(constant, disabled, 0, true);
    connectNodes(disabled, grade, 0, true);
    disabled->setNodeDisabled(true);

    const std::optional<std::array<float, 4>> rgba = sampleNodeInputAt(grade, 0, kTime, ViewIdx(0), 5., 5.);
    ASSERT_TRUE(rgba.has_value());
    EXPECT_NEAR(0.25f, (*rgba)[0], 1e-5f);
}

TEST_F(NodeColorSamplerTest, SampleInputAtWithoutAnInputIsNone)
{
    NodePtr grade = makeNode(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(bool(grade));

    EXPECT_FALSE(sampleNodeInputAt(grade, 0, kTime, ViewIdx(0), 5., 5.).has_value());
    EXPECT_FALSE(sampleNodeInputAt(grade, 7, kTime, ViewIdx(0), 5., 5.).has_value());
    EXPECT_FALSE(sampleNodeInputAt(NodePtr(), 0, kTime, ViewIdx(0), 5., 5.).has_value());
}
