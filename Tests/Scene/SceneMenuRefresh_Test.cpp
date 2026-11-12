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
#include <future>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>

#include "BaseTest.h"

#include "Engine/Nodes/Scene/SceneMenuRefresh.h"

NATRON_NAMESPACE_USING

namespace {

bool
processEventsUntil(const std::function<bool()>& done)
{
    QElapsedTimer timer;

    timer.start();
    while (!done()) {
        if (timer.elapsed() > 10000) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return true;
}

// Holds a Read on its pool thread until release().
struct Gate {
    std::promise<void> opened;
    std::shared_future<void> open { opened.get_future().share() };

    void release() { opened.set_value(); }

    bool wait() const { return open.wait_for(std::chrono::seconds(10)) == std::future_status::ready; }
};

} // anonymous namespace

class SceneMenuRefreshTest
    : public BaseTest {
};

TEST_F(SceneMenuRefreshTest, TheReadRunsOnAPoolThreadAndTheApplyOnTheMainThread)
{
    ASSERT_TRUE(QCoreApplication::instance() != nullptr);
    QObject context;
    SceneMenuRefresh refresh;
    auto gate = std::make_shared<Gate>();
    std::atomic<bool> readStarted(false);
    std::thread::id readThread;
    std::thread::id applyThread;
    const std::thread::id mainThread = std::this_thread::get_id();

    refresh.request(&context, [&]() -> SceneMenuRefresh::Read {
        return [&, gate]() -> SceneMenuRefresh::Apply {
            readThread = std::this_thread::get_id();
            readStarted = true;
            EXPECT_TRUE(gate->wait());

            return [&]() { applyThread = std::this_thread::get_id(); };
        };
    });
    // request() returned while the Read is held: it did not read on this thread.
    EXPECT_TRUE(refresh.isBusy());
    EXPECT_EQ(0u, refresh.getApplyCount());
    ASSERT_TRUE(processEventsUntil([&]() { return readStarted.load(); }));
    gate->release();
    ASSERT_TRUE(processEventsUntil([&]() { return refresh.getApplyCount() == 1; }));
    EXPECT_NE(mainThread, readThread);
    EXPECT_EQ(mainThread, applyThread);
    EXPECT_FALSE(refresh.isBusy());
}

TEST_F(SceneMenuRefreshTest, ARequestMadeWhileReadingDropsTheStaleResult)
{
    QObject context;
    SceneMenuRefresh refresh;
    auto gate = std::make_shared<Gate>();
    std::atomic<int> prepared(0);
    int applied = 0;
    const auto requestFor = [&](int value, const std::shared_ptr<Gate>& wait) {
        refresh.request(&context, [&, value, wait]() -> SceneMenuRefresh::Read {
            ++prepared;

            return [&, value, wait]() -> SceneMenuRefresh::Apply {
                if (wait) {
                    EXPECT_TRUE(wait->wait());
                }

                return [&, value]() { applied = value; };
            };
        });
    };

    requestFor(1, gate);
    requestFor(2, std::shared_ptr<Gate>());
    requestFor(3, std::shared_ptr<Gate>());
    EXPECT_EQ(1, prepared.load());
    gate->release();
    ASSERT_TRUE(processEventsUntil([&]() { return !refresh.isBusy(); }));
    // The first Read's result is stale and the second request was superseded before it began.
    EXPECT_EQ(2, prepared.load());
    EXPECT_EQ(1u, refresh.getApplyCount());
    EXPECT_EQ(3, applied);
}

TEST_F(SceneMenuRefreshTest, NothingAppliesOnceTheContextIsDestroyed)
{
    SceneMenuRefresh refresh;
    auto gate = std::make_shared<Gate>();
    std::atomic<bool> readEnded(false);
    bool applied = false;
    {
        QObject context;
        refresh.request(&context, [&, gate]() -> SceneMenuRefresh::Read {
            return [&, gate]() -> SceneMenuRefresh::Apply {
                EXPECT_TRUE(gate->wait());
                readEnded = true;

                return [&]() { applied = true; };
            };
        });
    }
    gate->release();
    ASSERT_TRUE(processEventsUntil([&]() { return readEnded.load() && !refresh.isBusy(); }));
    EXPECT_FALSE(applied);
    EXPECT_EQ(0u, refresh.getApplyCount());
}
