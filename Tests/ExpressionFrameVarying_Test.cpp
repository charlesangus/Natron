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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "CacheStress.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

using namespace CacheStress;

const RectI kWindow(0, 0, 16, 16);

struct GradeOverConstant {
    NodePtr constant;
    NodePtr grade;
    KnobIPtr white;
};

bool
build(const AppInstancePtr& app,
      GradeOverConstant* g)
{
    app->getProject()->setOrAddProjectFormat(Format(0, 0, kWindow.width(), kWindow.height(), "frameVarying", 1.));
    g->constant = createConstantIn(app, app->getProject(), kWindow, { 0.5, 0.5, 0.5, 1. });
    g->grade = createIn(app, app->getProject(), PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    if (!g->constant || !g->grade || !g->grade->connectInput(g->constant, 0)) {
        return false;
    }
    g->white = g->grade->getKnobByName(kGradeParamWhite);

    return g->white != nullptr;
}

bool
frameVarying(const NodePtr& node)
{
    return node->getEffectInstance()->isFrameVaryingOrAnimated_Recursive();
}

// Cached renders at frames 1 then 2, each compared with a render of the same frame from empty
// caches. Returns "" or what went wrong.
std::string
checkFrames(const AppInstancePtr& app,
            const NodePtr& probe,
            RenderSchedulerModeEnum mode)
{
    const double times[] = { 1., 2. };
    RenderedPlane refs[2];
    std::string error;

    for (int i = 0; i < 2; ++i) {
        clearEverything(app);
        if (!renderProbe(probe, times[i], 0, kWindow, mode, true, &refs[i], &error)) {
            return "the reference render failed: " + error;
        }
    }
    if (compareExact(refs[0], refs[1]).empty()) {
        return "frames 1 and 2 render the same, so the case shows nothing";
    }
    clearEverything(app);
    for (int i = 0; i < 2; ++i) {
        RenderedPlane got;
        if (!renderProbe(probe, times[i], 0, kWindow, mode, false, &got, &error)) {
            return "the cached render failed: " + error;
        }
        const std::string diff = compareExact(refs[i], got);
        if (!diff.empty()) {
            return std::string("frame ") + (i == 0 ? "1" : "2") + ": " + diff;
        }
    }

    return std::string();
}

const RenderSchedulerModeEnum kModes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };

} // namespace

class ExpressionFrameVaryingTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }
};

// An expression on frame with no keyframes keys the node's images on time.
TEST_F(ExpressionFrameVaryingTest, FrameExpressionIsFrameVarying)
{
    GradeOverConstant g;
    ASSERT_TRUE(build(getApp(), &g));
    EXPECT_FALSE(frameVarying(g.grade));
    g.white->setExpression(0, "frame*0.1", false, true);
    EXPECT_TRUE(frameVarying(g.grade));
    for (RenderSchedulerModeEnum mode : kModes) {
        EXPECT_EQ(std::string(), checkFrames(getApp(), g.grade, mode)) << "mode " << (int)mode;
    }
}

// A dimension slaved to a knob driven by a frame expression varies with time too, although the
// slave's own node never saw an expression or a keyframe change.
TEST_F(ExpressionFrameVaryingTest, SlavedToAFrameExpressionIsFrameVarying)
{
    GradeOverConstant g;
    ASSERT_TRUE(build(getApp(), &g));
    const NodePtr master = createIn(getApp(), getApp()->getProject(), PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(master != nullptr);
    const KnobIPtr masterWhite = master->getKnobByName(kGradeParamWhite);
    ASSERT_TRUE(masterWhite != nullptr);
    ASSERT_TRUE(g.white->slaveTo(0, masterWhite, 0));
    EXPECT_FALSE(frameVarying(g.grade));
    masterWhite->setExpression(0, "frame*0.1", false, true);
    EXPECT_TRUE(frameVarying(g.grade));
    for (RenderSchedulerModeEnum mode : kModes) {
        EXPECT_EQ(std::string(), checkFrames(getApp(), g.grade, mode)) << "mode " << (int)mode;
    }
}

// The answer follows the knobs as they are now, whatever was set and removed before, so a node
// with no expression and no keyframes over a static input shares one image across frames again.
TEST_F(ExpressionFrameVaryingTest, StaticAgainAfterTheExpressionAndKeysAreGone)
{
    GradeOverConstant g;
    ASSERT_TRUE(build(getApp(), &g));
    g.white->setExpression(0, "frame*0.1", false, true);
    ASSERT_TRUE(frameVarying(g.grade));
    g.white->clearExpression(0, true);
    EXPECT_FALSE(frameVarying(g.grade));

    KnobDoubleBase* white = dynamic_cast<KnobDoubleBase*>(g.white.get());
    ASSERT_TRUE(white != nullptr);
    white->setValueAtTime(1., 1., ViewSpec::all(), 0);
    white->setValueAtTime(2., 2., ViewSpec::all(), 0);
    EXPECT_TRUE(frameVarying(g.grade));
    g.white->removeAnimation(ViewSpec::all(), 0);
    EXPECT_FALSE(frameVarying(g.grade));

    clearEverything(getApp());
    RenderedPlane first;
    RenderedPlane second;
    std::string error;
    ASSERT_TRUE(renderProbe(g.grade, 1., 0, kWindow, eRenderSchedulerModeLegacy, false, &first, &error)) << error;
    ASSERT_TRUE(renderProbe(g.grade, 2., 0, kWindow, eRenderSchedulerModeLegacy, false, &second, &error)) << error;
    EXPECT_EQ(std::string(), compareExact(first, second));
}

// A dimension without an expression must not leave the holder's change bracket open, which would
// block every later evaluation of the node.
TEST_F(ExpressionFrameVaryingTest, RenameWithoutAnExpressionLeavesNoBracketOpen)
{
    GradeOverConstant g;
    ASSERT_TRUE(build(getApp(), &g));
    const EffectInstancePtr effect = g.grade->getEffectInstance();
    ASSERT_TRUE(g.white->getExpression(1).empty());

    g.white->replaceNodeNameInExpression(1, "oldName", "newName");
    EXPECT_FALSE(effect->isEvaluationBlocked());

    g.white->setExpression(0, "frame*0.1", false, true);
    g.white->replaceNodeNameInExpression(0, "frame", "frame");
    EXPECT_FALSE(effect->isEvaluationBlocked());
    EXPECT_EQ(std::string("frame*0.1"), g.white->getExpression(0));
}
