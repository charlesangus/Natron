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

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kSize = 64;

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
