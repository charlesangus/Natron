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

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>
#include <QThread>
#include <QThreadPool>

#include "BaseTest.h"

#include "Engine/AbortWatchdog.h"
#include "Engine/AbortableRenderInfo.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/ThreadPool.h"

NATRON_NAMESPACE_USING

namespace {
const int kTimeoutMs = 100;
const int kGuardMs = 10000;

// Runs fn on a fresh QThread, which has no event loop, and returns once that thread has exited.
void
runOnExitedThread(const std::function<void()>& fn)
{
    std::unique_ptr<QThread> thread(QThread::create(fn));
    thread->start();
    thread->wait();
}

// Spins the main thread's event loop, where the watchdog's timer lives, until done() holds.
bool
pumpUntil(const std::function<bool()>& done,
          int guardMs = kGuardMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done()) {
        if (elapsed.elapsed() > guardMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }

    return true;
}

struct StallReports {
    std::vector<std::string> reports;
    bool allOnMainThread = true;
};
} // namespace

class AbortableRenderInfoTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        AbortWatchdog::setTimeoutForTesting(kTimeoutMs);
        StallReports* reports = &_reports;
        AbortWatchdog::setStallReportHandlerForTesting([reports](const std::string& report) {
            reports->allOnMainThread = reports->allOnMainThread && QThread::currentThread() == QCoreApplication::instance()->thread();
            reports->reports.push_back(report);
        });
    }

    virtual void TearDown() OVERRIDE
    {
        // Drain what this test left behind so no report reaches the next test's handler.
        pumpUntil([]() { return AbortWatchdog::getNumWatchedForTesting() == 0; });
        AbortWatchdog::setStallReportHandlerForTesting(AbortWatchdog::StallReportHandler());
        AbortWatchdog::setTimeoutForTesting(0);
        BaseTest::TearDown();
    }

    // Creates and aborts a render on a pool thread while a thread is inside an action of node,
    // then lets the watchdog check it.
    void abortPoolRenderStalledIn(const NodePtr& node,
                                  bool unregisterBeforeTimeout)
    {
        AbortableThread actor(QThread::currentThread());
        actor.setCurrentActionInfos("render", node);

        AbortableRenderInfoPtr info;
        QThreadPool pool;
        pool.start([&info]() { info = AbortableRenderInfo::create(true, 0); });
        pool.waitForDone();
        ASSERT_TRUE(bool(info));

        actor.setAbortInfo(false, info, EffectInstancePtr());
        QElapsedTimer sinceAbort;
        sinceAbort.start();
        pool.start([&info]() { info->setAborted(); });
        pool.waitForDone();
        EXPECT_TRUE(AbortWatchdog::isWatchingForTesting(info));
        if (unregisterBeforeTimeout) {
            actor.clearAbortInfo();
        }

        const std::size_t reportsBefore = _reports.reports.size();
        ASSERT_TRUE(pumpUntil([&info]() { return !AbortWatchdog::isWatchingForTesting(info); }));
        EXPECT_GE(sinceAbort.elapsed(), kTimeoutMs);
        _newReports.assign(_reports.reports.begin() + reportsBefore, _reports.reports.end());

        actor.clearAbortInfo();
        actor.setCurrentActionInfos(std::string(), NodePtr());
    }

    StallReports _reports;
    std::vector<std::string> _newReports;
};

TEST_F(AbortableRenderInfoTest, DestroyedOnAnotherThreadAfterItsCreatingThreadExited)
{
    AbortableRenderInfoPtr info;
    runOnExitedThread([&info]() { info = AbortableRenderInfo::create(true, 0); });
    ASSERT_TRUE(bool(info));

    AbortableRenderInfoWPtr weak = info;
    runOnExitedThread([&info]() { info.reset(); });
    EXPECT_TRUE(weak.expired());
}

TEST_F(AbortableRenderInfoTest, AbortedOnAThirdThreadThenDestroyedOnAnother)
{
    AbortableRenderInfoPtr info;
    runOnExitedThread([&info]() { info = AbortableRenderInfo::create(true, 0); });
    ASSERT_TRUE(bool(info));

    runOnExitedThread([&info]() { info->setAborted(); });
    EXPECT_TRUE(info->isAborted());
    EXPECT_TRUE(AbortWatchdog::isWatchingForTesting(info));

    AbortableRenderInfoWPtr weak = info;
    runOnExitedThread([&info]() { info.reset(); });
    EXPECT_TRUE(weak.expired());

    // The watchdog drops the destroyed render on its next scan, before the timeout.
    EXPECT_TRUE(pumpUntil([]() { return AbortWatchdog::getNumWatchedForTesting() == 0; }));
    EXPECT_TRUE(_reports.reports.empty());
}

TEST_F(AbortableRenderInfoTest, CreateAbortDestroyAcrossPoolResizes)
{
    const int kCycles = 10000;
    const int kBatch = 100;
    const int poolSizes[] = { 16, 4, 1 };
    QThreadPool pool;
    // Idle threads exit at once, so most infos outlive the thread that created them.
    pool.setExpiryTimeout(0);

    for (int poolSize : poolSizes) {
        pool.setMaxThreadCount(poolSize);

        std::mutex createdMutex;
        std::vector<AbortableRenderInfoPtr> created;
        for (int batch = 0; batch < kCycles / kBatch; ++batch) {
            pool.start([&createdMutex, &created, batch]() {
                std::vector<AbortableRenderInfoPtr> mine;
                for (int i = 0; i < kBatch; ++i) {
                    AbortableRenderInfoPtr info = AbortableRenderInfo::create(true, 0);
                    if ((batch + i) % 2 == 0) {
                        info->setAborted();
                    }
                    mine.push_back(info);
                }
                std::lock_guard<std::mutex> k(createdMutex);
                created.insert(created.end(), mine.begin(), mine.end());
            });
        }
        pool.waitForDone();
        ASSERT_EQ((std::size_t)kCycles, created.size());

        for (int batch = 0; batch < kCycles / kBatch; ++batch) {
            pool.start([&createdMutex, &created]() {
                std::lock_guard<std::mutex> k(createdMutex);
                for (int i = 0; i < kBatch && !created.empty(); ++i) {
                    created.pop_back();
                }
            });
        }
        pool.waitForDone();
        EXPECT_TRUE(created.empty()) << "pool size " << poolSize;
        // Let the watchdog scan while infos are being destroyed elsewhere.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    EXPECT_TRUE(pumpUntil([]() { return AbortWatchdog::getNumWatchedForTesting() == 0; }));
    EXPECT_TRUE(_reports.reports.empty());
}

TEST_F(AbortableRenderInfoTest, AStalledPoolRenderIsReportedOnTheMainThread)
{
    NodePtr checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    ASSERT_TRUE(bool(checker));

    abortPoolRenderStalledIn(checker, false);
    ASSERT_EQ((std::size_t)1, _newReports.size());
    EXPECT_NE(std::string::npos, _newReports[0].find(checker->getFullyQualifiedName()));
    EXPECT_NE(std::string::npos, _newReports[0].find("Action: render"));
    EXPECT_TRUE(_reports.allOnMainThread);
}

TEST_F(AbortableRenderInfoTest, UnregisteringEveryThreadBeforeTheTimeoutSuppressesTheReport)
{
    NodePtr checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    ASSERT_TRUE(bool(checker));

    abortPoolRenderStalledIn(checker, true);
    EXPECT_TRUE(_newReports.empty());
}

TEST_F(AbortableRenderInfoTest, ReadersAndWritersNeverReport)
{
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
    ASSERT_TRUE(bool(reader));
    ASSERT_TRUE(reader->getEffectInstance()->isReader());
    NodePtr writer = createNativeWriter();
    ASSERT_TRUE(bool(writer));
    ASSERT_TRUE(writer->getEffectInstance()->isWriter());

    abortPoolRenderStalledIn(reader, false);
    EXPECT_TRUE(_newReports.empty());
    abortPoolRenderStalledIn(writer, false);
    EXPECT_TRUE(_newReports.empty());
}
