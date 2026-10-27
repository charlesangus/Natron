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

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <QCoreApplication>

#include "BaseTest.h"
#include "CacheStress.h"
#include "CacheStressTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
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

// A value callback in the dequeue can start a render on the main thread, which does not wait for
// the dequeue it runs in: the values already dequeued stand, and the inputs stay queued until
// that render ends.
TEST_F(QueuedInputEditTest, InputsStayQueuedUnderARenderStartedByAValueCallback)
{
    const AppInstancePtr app = getApp();
    app->getProject()->setOrAddProjectFormat(Format(0, 0, kWindow.width(), kWindow.height(), "queuedInputEdit", 1.));
    const NodePtr a = createConstantIn(app, app->getProject(), kWindow, { 0.5, 0.25, 0.125, 1. });
    const NodePtr b = createConstantIn(app, app->getProject(), kWindow, { 0.125, 0.75, 0.5, 1. });
    const NodePtr node = createIn(app, app->getProject(), kTestPluginIDQueuedEdit);
    ASSERT_TRUE(bool(a) && bool(b) && bool(node));
    ASSERT_TRUE(node->connectInput(a, 0));
    std::shared_ptr<QueuedEditTestEffect> effect = std::dynamic_pointer_cast<QueuedEditTestEffect>(node->getEffectInstance());
    ASSERT_TRUE(bool(effect));
    KnobDoublePtr value = effect->valueKnob();
    ASSERT_TRUE(bool(value));

    std::unique_ptr<RenderingFlagSetter> first(new RenderingFlagSetter(node));
    value->setValue(2.);
    EXPECT_EQ(1., value->getValue());
    ASSERT_EQ(0, node->disconnectInput(0));
    ASSERT_TRUE(node->connectInput(b, 0));
    EXPECT_EQ(a, node->getInput(0));
    EXPECT_EQ(b, node->getGuiInput(0));

    std::unique_ptr<RenderingFlagSetter> second;
    effect->valueChanged = [&second, node]() {
        if (!second) {
            second.reset(new RenderingFlagSetter(node));
        }
    };
    releaseOnAWorker(&first);
    QCoreApplication::processEvents();
    effect->valueChanged = std::function<void()>();
    ASSERT_TRUE(bool(second)) << "the dequeue did not run the value callback";
    EXPECT_EQ(2., value->getValue());
    EXPECT_EQ(a, node->getInput(0));
    EXPECT_EQ(b, node->getGuiInput(0));

    releaseOnAWorker(&second);
    QCoreApplication::processEvents();
    EXPECT_EQ(b, node->getInput(0));
    EXPECT_EQ(b, node->getGuiInput(0));
    EXPECT_EQ(2., value->getValue());
}

// An input edit that found no render holds the node keeps render starts out until it has
// applied: a render that tries to start in between waits, then sees the new input.
TEST_F(QueuedInputEditTest, ARenderCannotStartBetweenTheCheckAndTheApply)
{
    TwoSources g;
    ASSERT_TRUE(build(getApp(), &g));

    std::atomic<bool> armed(true);
    std::atomic<bool> starting(false);
    std::atomic<bool> acquired(false);
    bool hookRan = false;
    bool hookSawRender = false;
    bool heldOff = false;
    std::unique_ptr<RenderingFlagSetter> flag;
    NodePtr seen;
    std::thread worker;
    const NodePtr grade = g.grade;
    Node::setInputEditHookForTesting([&](const Node* node,
                                         bool queued) {
        if ((node != grade.get()) || !armed.exchange(false)) {
            return;
        }
        hookRan = true;
        hookSawRender = queued;
        worker = std::thread([&]() {
            starting = true;
            flag.reset(new RenderingFlagSetter(grade));
            acquired = true;
            seen = grade->getInput(0);
        });
        while (!starting) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        heldOff = !acquired;
    });
    const bool replaced = grade->replaceInput(g.b, 0);
    if (worker.joinable()) {
        worker.join();
    }
    Node::setInputEditHookForTesting(Node::InputEditHook());

    EXPECT_TRUE(replaced);
    EXPECT_TRUE(hookRan);
    EXPECT_FALSE(hookSawRender);
    EXPECT_TRUE(heldOff) << "the render started while the edit was between its check and its apply";
    EXPECT_TRUE(acquired);
    EXPECT_EQ(g.b, seen);
    EXPECT_EQ(g.b, grade->getInput(0));
    EXPECT_EQ(g.b, grade->getGuiInput(0));

    releaseOnAWorker(&flag);
    QCoreApplication::processEvents();
    EXPECT_EQ(g.b, grade->getInput(0));
}
