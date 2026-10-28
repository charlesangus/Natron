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

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "CacheStress.h"
#include "CacheStressTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CLArgs.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_USING

namespace {

using namespace CacheStress;

const RectI kWindow(0, 0, 48, 32);

// StaleMemo -> Grade, probing the Grade.
bool
buildPlantedHole(const AppInstancePtr& app,
                 StressGraph* g,
                 std::string* error)
{
    const NodeCollectionPtr root = app->getProject();
    const int memo = addNode(g, createIn(app, root, kTestPluginIDStaleMemo));
    const int grade = addNode(g, createIn(app, root, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE));

    if ((memo < 0) || (grade < 0)) {
        *error = "could not create the nodes";

        return false;
    }
    if (!g->nodes[grade]->connectInput(g->nodes[memo], 0)) {
        *error = "could not connect the grade";

        return false;
    }
    addKnob(g, memo, "value", 0, { 1., 2., 3. }, false);
    addKnob(g, grade, kGradeParamWhite, 0, { 1., 0.5, 2. });
    g->probe = grade;
    g->window = RectI(0, 0, kStaleMemoTestSize, kStaleMemoTestSize);

    return true;
}

// Two Constants feeding Grade -> Blur, with keyframes, per-dimension expressions and a link.
bool
buildChain(const AppInstancePtr& app,
           StressGraph* g,
           std::string* error)
{
    const NodeCollectionPtr root = app->getProject();
    const int constant = addNode(g, createConstantIn(app, root, kWindow, { 0.25, 0.5, 0.75, 1. }));
    const int other = addNode(g, createConstantIn(app, root, kWindow, { 0.9, 0.1, 0.4, 1. }));
    const int grade = addNode(g, createIn(app, root, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE));
    const int blur = addNode(g, createIn(app, root, PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR));

    if ((constant < 0) || (other < 0) || (grade < 0) || (blur < 0)) {
        *error = "could not create the nodes";

        return false;
    }
    if (!g->nodes[grade]->connectInput(g->nodes[constant], 0) || !g->nodes[blur]->connectInput(g->nodes[grade], 0)) {
        *error = "could not connect the chain";

        return false;
    }
    addSlot(g, grade, 0, { constant, other }, false);
    addKnob(g, constant, kConstantParamColor, 0, { 0.25, 0.6, 1.5 });
    const int white = addKnob(g, grade, kGradeParamWhite, 0, { 1., 0.5, 2. });
    const int multiply = addKnob(g, grade, kGradeParamMultiply, 1, { 1., 0.8, 1.25 });
    addKnob(g, blur, kBlurParamSize, 0, { 0., 1., 3. }, false);
    addKnob(g, other, kConstantParamColor, 1, { 0.1, 0.3 }, false);
    g->exprTargets = { white, multiply };
    g->links.push_back(std::make_pair(white, multiply));
    g->toggles = { grade, blur };
    g->probe = blur;
    g->window = kWindow;

    return true;
}

} // namespace

class CacheStressSelfTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }
};

// The harness must notice a cache the engine does not know about: a source that reuses its
// first render's value. The initial render fills it, the scripted edit changes the value, and
// only the reference forgets.
TEST_F(CacheStressSelfTest, APlantedMemberCacheHoleIsCaught)
{
    Options options;
    options.ops = opBit(eOpSetValue);
    options.seeded = false;
    options.scripted.resize(1);
    options.scripted[0].kind = eOpSetValue;
    options.scripted[0].prims.push_back(setValuePrim(0, 0, 2.));

    for (int mode = 0; mode < 2; ++mode) {
        options.legacy = mode == 0;
        options.taskGraph = mode == 1;
        const Result r = run(getApp(), buildPlantedHole, 7u, options);
        EXPECT_FALSE(r.ok) << (mode == 0 ? "legacy" : "task graph");
        EXPECT_EQ(1, r.failedStep) << r.report;
        EXPECT_NE(std::string::npos, r.report.find("NATRON_CACHE_STRESS_SEED=7")) << r.report;
        EXPECT_NE(std::string::npos, r.report.find("setValue")) << r.report;
    }
}

// The seeded walk finds the planted hole too, not only the script: with half the edits on the
// memo's knob, eight steps all but surely reach it.
TEST_F(CacheStressSelfTest, ASeededWalkCatchesThePlantedHole)
{
    Options options;
    options.ops = opBit(eOpSetValue);
    options.steps = 8;
    const Result r = run(getApp(), buildPlantedHole, 11u, options);
    EXPECT_FALSE(r.ok);
    EXPECT_GT(r.failedStep, 0) << r.report;
}

TEST_F(CacheStressSelfTest, AHoleFreeGraphPasses)
{
    Options options;
    options.ops = defaultOps();
    options.steps = 20;
    EXPECT_EQ(0, expectClean(getApp(), buildChain, { 1u, 2u }, options));
}

TEST_F(CacheStressSelfTest, ASeedReplaysTheSameRun)
{
    Options options;
    options.ops = defaultOps();
    options.steps = 10;
    const Result a = run(getApp(), buildChain, 5u, options);
    const Result b = run(getApp(), buildChain, 5u, options);
    EXPECT_EQ(a.ok, b.ok);
    EXPECT_EQ(a.failedStep, b.failedStep);
    EXPECT_EQ(a.report, b.report);
}

// The reference is what the project means: a project reset and reloaded from the saved file renders the
// same pixels from cold caches.
TEST_F(CacheStressSelfTest, TheReferenceEqualsAFreshAppLoadOfTheSavedProject)
{
    const AppInstancePtr app = getApp();
    StressGraph g;
    std::string error;
    ASSERT_TRUE(buildChain(app, &g, &error)) << error;

    const NodePtr grade = g.nodes[2];
    const NodePtr blur = g.nodes[3];
    std::shared_ptr<KnobDoubleBase> white = std::dynamic_pointer_cast<KnobDoubleBase>(grade->getKnobByName(kGradeParamWhite));
    std::shared_ptr<KnobDoubleBase> multiply = std::dynamic_pointer_cast<KnobDoubleBase>(grade->getKnobByName(kGradeParamMultiply));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
    ASSERT_TRUE(bool(white) && bool(multiply) && bool(size));
    white->setValueAtTime(1, 0.5, ViewSpec::all(), 0);
    white->setValueAtTime(5, 2., ViewSpec::all(), 0);
    ASSERT_NO_THROW(multiply->setExpression(1, blur->getScriptName() + "." kBlurParamSize ".getValue(0)", false, true));
    size->setValue(2., ViewSpec::all(), 0);
    size->setValue(2., ViewSpec::all(), 1);
    app->getTimeLine()->seekFrame(3, false, NULL, eTimelineChangeReasonOtherSeek);

    clearEverything(app);
    RenderedPlane reference;
    ASSERT_TRUE(renderProbe(blur, 3., 0, kWindow, eRenderSchedulerModeLegacy, true, &reference, &error)) << error;

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("cache-stress-reference.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(app->getProject()->saveProject(dirPath, fileName, &savedFilePath));
    const std::string blurName = blur->getScriptName();

    const ProjectPtr project = app->getProject();
    project->reset(false, true);
    ASSERT_TRUE(project->getNodeByName(blurName) == nullptr);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));
    const NodePtr loaded = project->getNodeByName(blurName);
    ASSERT_TRUE(bool(loaded));

    for (int mode = 0; mode < 2; ++mode) {
        clearEverything(app);
        RenderedPlane fresh;
        ASSERT_TRUE(renderProbe(loaded, 3., 0, kWindow, mode == 0 ? eRenderSchedulerModeLegacy : eRenderSchedulerModeTaskGraph, true, &fresh, &error)) << error;
        EXPECT_EQ(std::string(), compareExact(reference, fresh)) << (mode == 0 ? "legacy" : "task graph");
    }
}
