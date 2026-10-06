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
#include <set>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kConstantPluginID = "net.sf.openfx.ConstantPlugin";
const char* const kGradePluginID = "net.sf.openfx.GradePlugin";

} // namespace

class GraphScalingExpressionDeps
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }
};

TEST_F(GraphScalingExpressionDeps, DependencyGetsFrameArgs)
{
    NodePtr unconnected = createNode(QString::fromUtf8(kConstantPluginID));
    NodePtr input = createNode(QString::fromUtf8(kConstantPluginID));
    NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(unconnected && input && grade);
    connectNodes(input, grade, 0, true);

    KnobIPtr offset = grade->getKnobByName("offset");
    ASSERT_TRUE(bool(offset));
    ASSERT_GE(offset->getDimension(), 2);
    ASSERT_NO_THROW(offset->setExpression(0, unconnected->getScriptName() + ".color.getValue(0)", false, true));
    ASSERT_NO_THROW(offset->setExpression(1, input->getScriptName() + ".color.getValue(1)", false, true));

    {
        std::set<NodePtr> deps;
        grade->getEffectInstance()->getAllExpressionDependenciesRecursive(deps);
        ASSERT_EQ(1u, deps.count(unconnected));
        ASSERT_EQ(1u, deps.count(input));
    }

    EffectInstancePtr unconnectedEffect = unconnected->getEffectInstance();
    EffectInstancePtr inputEffect = input->getEffectInstance();
    EffectInstancePtr gradeEffect = grade->getEffectInstance();
    ASSERT_TRUE(unconnectedEffect && inputEffect && gradeEffect);
    EXPECT_FALSE(bool(unconnectedEffect->getParallelRenderArgsTLS()));
    EXPECT_FALSE(bool(inputEffect->getParallelRenderArgsTLS()));

    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        ParallelRenderArgsSetter setter(1.,
                                        ViewIdx(0),
                                        false,
                                        false,
                                        abortInfo,
                                        grade,
                                        0,
                                        getApp()->getTimeLine().get(),
                                        NodePtr(),
                                        false,
                                        false,
                                        RenderStatsPtr());

        ParallelRenderArgsPtr gradeArgs = gradeEffect->getParallelRenderArgsTLS();
        ASSERT_TRUE(bool(gradeArgs));
        EXPECT_EQ(1, gradeArgs->visitsCount);

        ParallelRenderArgsPtr depArgs = unconnectedEffect->getParallelRenderArgsTLS();
        ASSERT_TRUE(bool(depArgs)) << "a node read only through an expression got no frame args";
        EXPECT_EQ(grade.get(), depArgs->treeRoot.get());
        EXPECT_EQ(1., depArgs->time);
        // Not part of the image tree, so the render never visits it.
        EXPECT_EQ(0, depArgs->visitsCount);

        // The setter's node set is private, so a node reachable both as an input and through an expression is
        // checked to be counted once by its visit count.
        ParallelRenderArgsPtr inputArgs = inputEffect->getParallelRenderArgsTLS();
        ASSERT_TRUE(bool(inputArgs));
        EXPECT_EQ(grade.get(), inputArgs->treeRoot.get());
        EXPECT_EQ(1, inputArgs->visitsCount);
    }

    EXPECT_FALSE(bool(unconnectedEffect->getParallelRenderArgsTLS()));
    EXPECT_FALSE(bool(inputEffect->getParallelRenderArgsTLS()));
    EXPECT_FALSE(bool(gradeEffect->getParallelRenderArgsTLS()));
}
