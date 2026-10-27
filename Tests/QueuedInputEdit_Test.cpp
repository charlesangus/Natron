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
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <QCoreApplication>

#include "BaseTest.h"
#include "CacheStress.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

using namespace CacheStress;

const RectI kWindow(0, 0, 16, 16);
const double kTime = 1.;

struct TwoSources {
    NodePtr a;
    NodePtr b;
    NodePtr grade;
};

bool
build(const AppInstancePtr& app,
      TwoSources* g)
{
    app->getProject()->setOrAddProjectFormat(Format(0, 0, kWindow.width(), kWindow.height(), "queuedInputEdit", 1.));
    g->a = createConstantIn(app, app->getProject(), kWindow, { 0.5, 0.25, 0.125, 1. });
    g->b = createConstantIn(app, app->getProject(), kWindow, { 0.125, 0.75, 0.5, 1. });
    g->grade = createIn(app, app->getProject(), PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);

    return g->a && g->b && g->grade && g->grade->connectInput(g->a, 0);
}

// Disconnects input 0 and connects source there; both are queued while the grade renders.
bool
switchTo(const TwoSources& g,
         const NodePtr& source)
{
    return (g.grade->disconnectInput(0) == 0) && g.grade->connectInput(source, 0);
}

bool
reference(const AppInstancePtr& app,
          const NodePtr& probe,
          RenderedPlane* out,
          std::string* error)
{
    clearEverything(app);

    return renderProbe(probe, kTime, 0, kWindow, eRenderSchedulerModeLegacy, true, out, error);
}

std::string
cachedDiff(const NodePtr& probe,
           const RenderedPlane& ref)
{
    RenderedPlane got;
    std::string error;

    if (!renderProbe(probe, kTime, 0, kWindow, eRenderSchedulerModeLegacy, false, &got, &error)) {
        return "the cached render failed: " + error;
    }

    return compareExact(ref, got);
}

// Ends a render off the main thread, the way render threads do, so the dequeue it asks for is
// posted to the main thread instead of running at once.
void
releaseOnAWorker(std::unique_ptr<RenderingFlagSetter>* flag)
{
    std::thread t([flag]() { flag->reset(); });
    t.join();
}

} // namespace

class QueuedInputEditTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }
};

// A render that starts after the queued edit asked for its dequeue, but before the main thread
// ran it, keeps the inputs its hash names; the edit applies once that render ends too.
TEST_F(QueuedInputEditTest, NotAppliedUnderARenderThatStartedAfterTheRequest)
{
    TwoSources g;
    ASSERT_TRUE(build(getApp(), &g));
    RenderedPlane refA;
    RenderedPlane refB;
    std::string error;
    ASSERT_TRUE(reference(getApp(), g.grade, &refA, &error)) << error;
    ASSERT_TRUE(switchTo(g, g.b));
    ASSERT_TRUE(reference(getApp(), g.grade, &refB, &error)) << error;
    ASSERT_TRUE(switchTo(g, g.a));
    ASSERT_FALSE(compareExact(refA, refB).empty());
    clearEverything(getApp());
    const U64 hashA = g.grade->getHashValue();

    std::unique_ptr<RenderingFlagSetter> first(new RenderingFlagSetter(g.grade));
    ASSERT_TRUE(switchTo(g, g.b));
    EXPECT_EQ(g.a, g.grade->getInput(0));
    EXPECT_EQ(g.b, g.grade->getGuiInput(0));
    releaseOnAWorker(&first);

    std::unique_ptr<RenderingFlagSetter> second(new RenderingFlagSetter(g.grade));
    QCoreApplication::processEvents();
    EXPECT_EQ(g.a, g.grade->getInput(0));
    EXPECT_EQ(hashA, g.grade->getHashValue());
    EXPECT_EQ(std::string(), cachedDiff(g.grade, refA));

    releaseOnAWorker(&second);
    QCoreApplication::processEvents();
    EXPECT_EQ(g.b, g.grade->getInput(0));
    EXPECT_NE(hashA, g.grade->getHashValue());
    EXPECT_EQ(std::string(), cachedDiff(g.grade, refB));

    // Back to A: the entry stored while the edit was pending must be A's.
    ASSERT_TRUE(switchTo(g, g.a));
    EXPECT_EQ(hashA, g.grade->getHashValue());
    EXPECT_EQ(std::string(), cachedDiff(g.grade, refA));
}

// The last render to end applies the queue, in a background app as well.
TEST_F(QueuedInputEditTest, AppliedWhenTheLastRenderEnds)
{
    TwoSources g;
    ASSERT_TRUE(build(getApp(), &g));

    std::unique_ptr<RenderingFlagSetter> outer(new RenderingFlagSetter(g.grade));
    std::unique_ptr<RenderingFlagSetter> inner(new RenderingFlagSetter(g.grade));
    ASSERT_TRUE(switchTo(g, g.b));
    inner.reset();
    QCoreApplication::processEvents();
    EXPECT_EQ(g.a, g.grade->getInput(0));
    outer.reset();
    QCoreApplication::processEvents();
    EXPECT_EQ(g.b, g.grade->getInput(0));
    EXPECT_EQ(g.b, g.grade->getGuiInput(0));
}
