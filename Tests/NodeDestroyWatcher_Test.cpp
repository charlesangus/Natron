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
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/GenericSchedulerThreadWatcher.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

enum TestTaskEnum {
    eTestTaskBlockUntilReleased,
    eTestTaskRecord,
    eTestTaskStopOwnWatcher
};

class TestWatcher
    : public GenericWatcher {
public:
    QSemaphore entered;
    QSemaphore release;
    std::mutex ranMutex;
    std::vector<int> ran;

    std::vector<int> ranTasks()
    {
        std::lock_guard<std::mutex> l(ranMutex);

        return ran;
    }

private:
    virtual void handleBlockingTask(int taskID) OVERRIDE FINAL
    {
        switch ((TestTaskEnum)taskID) {
        case eTestTaskBlockUntilReleased:
            entered.release();
            release.acquire();
            break;
        case eTestTaskRecord:
            break;
        case eTestTaskStopOwnWatcher:
            stopWatching();
            finishPendingTasksAndStop();
            break;
        }
        std::lock_guard<std::mutex> l(ranMutex);
        ran.push_back(taskID);
    }
};

} // namespace

TEST(GenericWatcherStop, FinishRunsTasksQueuedBehindARunningOne)
{
    TestWatcher watcher;
    std::atomic<int> finished(0);
    QObject::connect(&watcher, &GenericWatcher::taskFinished, [&finished](int, GenericWatcherCallerArgsPtr) {
        ++finished;
    });

    watcher.scheduleBlockingTask(eTestTaskBlockUntilReleased);
    watcher.entered.acquire();
    watcher.scheduleBlockingTask(eTestTaskRecord);

    // Released after the stop below has been requested, while the second task is still queued.
    std::thread releaser([&watcher] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        watcher.release.release();
    });
    watcher.finishPendingTasksAndStop();
    releaser.join();

    EXPECT_FALSE(watcher.isRunning());
    EXPECT_EQ((std::vector<int> { eTestTaskBlockUntilReleased, eTestTaskRecord }), watcher.ranTasks());
    EXPECT_EQ(2, finished.load());
}

TEST(GenericWatcherStop, StopFromTheWatcherThreadReturns)
{
    TestWatcher watcher;

    watcher.scheduleBlockingTask(eTestTaskStopOwnWatcher);
    watcher.scheduleBlockingTask(eTestTaskRecord);
    watcher.finishPendingTasksAndStop();

    EXPECT_FALSE(watcher.isRunning());
    EXPECT_EQ((std::vector<int> { eTestTaskStopOwnWatcher, eTestTaskRecord }), watcher.ranTasks());
}

class NodeDestroyWatcherTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createDot(const NodeCollectionPtr& parent)
    {
        CreateNodeArgs args(PLUGINID_NATRON_DOT, parent);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    NodePtr createEmptyGroup()
    {
        CreateNodeArgs args(PLUGINID_NATRON_GROUP, getApp()->getProject());
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    static void processEventsUntil(const std::function<bool()>& done)
    {
        QElapsedTimer timer;
        timer.start();
        while (!done() && (timer.elapsed() < 10000)) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

// A fresh node's preview thread is not flagged as quit, so destroyNode(false) always goes through
// a watcher. No event is processed before the blocking destroy, as at app exit.
TEST_F(NodeDestroyWatcherTest, BlockingDestroyAfterNonBlockingOneReleasesTheNode)
{
    NodePtr node = createDot(getApp()->getProject());
    ASSERT_TRUE(bool(node));
    NodeWPtr weak = node;

    node->destroyNode(false, false);
    node->destroyNode(true, false);

    EXPECT_FALSE(bool(node->getEffectInstance()));
    node.reset();
    EXPECT_TRUE(weak.expired()) << "the destroy watcher still holds the node";

    // The watcher's taskFinished() may still be queued: delivering it must be harmless.
    QCoreApplication::processEvents();
}

TEST_F(NodeDestroyWatcherTest, ChildBlockingDestroyLeavesGroupNonBlockingDestroyToFinish)
{
    NodePtr group = createEmptyGroup();
    ASSERT_TRUE(bool(group));
    NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
    ASSERT_TRUE(bool(collection));
    NodePtr child = createDot(collection);
    ASSERT_TRUE(bool(child));
    NodeWPtr groupWeak = group;
    NodeWPtr childWeak = child;
    collection.reset();

    group->destroyNode(false, false);
    child->destroyNode(true, false);
    EXPECT_FALSE(bool(child->getEffectInstance()));

    processEventsUntil([&group] { return !group->getEffectInstance(); });
    EXPECT_FALSE(bool(group->getEffectInstance())) << "the group's requested destroy never completed";

    child.reset();
    group.reset();
    processEventsUntil([&groupWeak] { return groupWeak.expired(); });
    EXPECT_TRUE(groupWeak.expired());
    EXPECT_TRUE(childWeak.expired());
}
