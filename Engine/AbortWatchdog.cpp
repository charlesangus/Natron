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

#include "AbortWatchdog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppManager.h"
#include "Engine/Timer.h"

// After this amount of time, if any thread identified in an aborted render is still remaining
// that means they are stuck probably doing a long processing that cannot be aborted or in a separate thread
// that we did not spawn. Anyway, report to the user that we cannot control this thread anymore and that it may
// waste resources.
#define NATRON_ABORT_TIMEOUT_MS 5000

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

typedef std::chrono::steady_clock Clock;

struct WatchedRender {
    AbortableRenderInfoWPtr info;
    Clock::time_point abortedAt;
};

struct WatchdogState;
void stopThread(WatchdogState& s);

struct WatchdogState {
    std::mutex mutex;
    std::condition_variable wake;
    // Renders stay listed until their check is over, report included.
    std::vector<WatchedRender> renders;
    // Destroyed renders are only pruned by a scan, which does not run while a stall dialog is
    // open; pruning on insertion past this size keeps the list bounded meanwhile.
    std::size_t pruneThreshold = 64;
    AbortWatchdog::StallReportHandler handler;
    std::thread thread;
    // Set for good once the application shuts the watchdog down.
    bool stopping = false;
    // While the stall dialog is open on the main thread, scans wait so dialogs never stack.
    bool dialogOpen = false;

    // Held by the watchdog thread while it checks due renders and reports them, so that replacing
    // the handler waits for a report in flight. Taken before mutex, never after.
    std::mutex reportMutex;

    ~WatchdogState()
    {
        stopThread(*this);
    }
};

WatchdogState&
state()
{
    static WatchdogState s;

    return s;
}

std::atomic<int> s_timeoutOverrideMs(0);

int
scanIntervalMs()
{
    return std::max(1, AbortWatchdog::getTimeoutMs() / 4);
}

void
stopThread(WatchdogState& s)
{
    std::thread thread;
    {
        std::lock_guard<std::mutex> k(s.mutex);
        s.stopping = true;
        thread.swap(s.thread);
    }
    s.wake.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
}

void
showStallDialog(const std::vector<AbortableRenderInfoPtr>& stalled,
                const std::string& message)
{
    WatchdogState& s = state();
    StandardButtonEnum reply = Dialogs::questionDialog(AbortWatchdog::tr("A Render is not responding anymore").toStdString(), message, false, StandardButtons(eStandardButtonYes | eStandardButtonNo), eStandardButtonNo);

    if (reply == eStandardButtonYes) {
        for (std::size_t i = 0; i < stalled.size(); ++i) {
            stalled[i]->killRegisteredThreads();
        }
    }
    {
        std::lock_guard<std::mutex> k(s.mutex);
        s.dialogOpen = false;
    }
    s.wake.notify_all();
}

// Runs on the watchdog thread, with reportMutex held.
void
report(const std::vector<AbortableRenderInfoPtr>& stalled,
       const std::string& details)
{
    WatchdogState& s = state();
    const int timeoutMs = AbortWatchdog::getTimeoutMs();
    const QString timeoutStr = Timer::printAsTime(timeoutMs / 1000., false);
    std::stringstream ss;

    ss << AbortWatchdog::tr("One or multiple render seems to not be responding anymore after numerous attempt made by %1 to abort them for the last %2.").arg(QString::fromUtf8(NATRON_APPLICATION_NAME)).arg(timeoutStr).toStdString() << std::endl;
    ss << AbortWatchdog::tr("This is likely due to a render taking too long in a plug-in.").toStdString() << std::endl
       << std::endl;
    ss << std::endl;
    ss << AbortWatchdog::tr("List of stalled render(s):").toStdString() << std::endl
       << std::endl;
    ss << details;

    AbortWatchdog::StallReportHandler handler;
    {
        std::lock_guard<std::mutex> k(s.mutex);
        handler = s.handler;
    }
    if (handler) {
        handler(ss.str());

        return;
    }
    // The application is torn down only after the watchdog thread was stopped.
    QCoreApplication* app = QCoreApplication::instance();
    if (!app || !appPTR || appPTR->isBackground()) {
        qDebug() << ss.str().c_str();

        return;
    }

    ss << AbortWatchdog::tr("Would you like to kill these renders?").toStdString() << std::endl
       << std::endl;
    ss << AbortWatchdog::tr("WARNING: Killing them may not work or may leave %1 in a bad state. The application may crash or freeze as a consequence of this. It is advised to restart %1 instead.").arg(QString::fromUtf8(NATRON_APPLICATION_NAME)).toStdString();

    {
        std::lock_guard<std::mutex> k(s.mutex);
        s.dialogOpen = true;
    }
    const std::string message = ss.str();
    QMetaObject::invokeMethod(app, [stalled, message]() { showStallDialog(stalled, message); }, Qt::QueuedConnection);
} // report

// Runs on the watchdog thread, with reportMutex held and the state's mutex released.
void
checkDueRenders(const std::vector<AbortableRenderInfoPtr>& due)
{
    std::vector<AbortableRenderInfoPtr> stalled;
    std::string details;

    for (std::size_t i = 0; i < due.size(); ++i) {
        std::string renderDetails;
        if (due[i]->getStallReport(&renderDetails)) {
            stalled.push_back(due[i]);
            details += renderDetails;
        }
    }
    if (!stalled.empty()) {
        report(stalled, details);
    }
}

// The watchdog thread's body. It only wakes up while renders are watched and no dialog is open,
// so it costs nothing otherwise. Being its own thread, it times out renders even while the main
// thread is blocked, which a command-line render or a script's app.render() does.
void
run()
{
    WatchdogState& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);

    for (;;) {
        if (s.stopping) {
            return;
        }
        if (s.renders.empty() || s.dialogOpen) {
            s.wake.wait(lock);
            continue;
        }
        s.wake.wait_for(lock, std::chrono::milliseconds(scanIntervalMs()));
        if (s.stopping) {
            return;
        }
        if (s.dialogOpen) {
            continue;
        }

        const Clock::time_point now = Clock::now();
        const Clock::duration timeout = std::chrono::milliseconds(AbortWatchdog::getTimeoutMs());
        std::vector<AbortableRenderInfoPtr> due;
        std::vector<WatchedRender>::iterator kept = s.renders.begin();
        for (std::vector<WatchedRender>::iterator it = s.renders.begin(); it != s.renders.end(); ++it) {
            AbortableRenderInfoPtr info = it->info.lock();
            if (!info) {
                continue;
            }
            if (now - it->abortedAt >= timeout) {
                due.push_back(info);
            }
            *kept++ = *it;
        }
        s.renders.erase(kept, s.renders.end());
        if (due.empty()) {
            continue;
        }

        lock.unlock();
        {
            std::lock_guard<std::mutex> k(s.reportMutex);
            checkDueRenders(due);
        }
        lock.lock();
        s.renders.erase(std::remove_if(s.renders.begin(), s.renders.end(), [&due](const WatchedRender& r) {
                            AbortableRenderInfoPtr info = r.info.lock();

                            return !info || std::find(due.begin(), due.end(), info) != due.end();
                        }),
                        s.renders.end());
    }
} // run

NATRON_NAMESPACE_ANONYMOUS_EXIT

void
AbortWatchdog::watch(const AbortableRenderInfoWPtr& info)
{
    WatchdogState& s = state();
    bool wasIdle;
    {
        std::lock_guard<std::mutex> k(s.mutex);
        if (s.stopping) {
            return;
        }
        if (s.renders.size() >= s.pruneThreshold) {
            s.renders.erase(std::remove_if(s.renders.begin(), s.renders.end(), [](const WatchedRender& r) { return r.info.expired(); }), s.renders.end());
            s.pruneThreshold = std::max<std::size_t>(64, s.renders.size() * 2);
        }
        WatchedRender render;
        render.info = info;
        render.abortedAt = Clock::now();
        wasIdle = s.renders.empty();
        s.renders.push_back(render);
        if (!s.thread.joinable()) {
            s.thread = std::thread(run);
        }
    }
    if (wasIdle) {
        s.wake.notify_all();
    }
}

void
AbortWatchdog::shutdown()
{
    stopThread(state());
}

int
AbortWatchdog::getTimeoutMs()
{
    const int timeoutOverride = s_timeoutOverrideMs.load();

    return timeoutOverride > 0 ? timeoutOverride : NATRON_ABORT_TIMEOUT_MS;
}

void
AbortWatchdog::setTimeoutForTesting(int milliseconds)
{
    s_timeoutOverrideMs.store(milliseconds);
}

void
AbortWatchdog::setStallReportHandlerForTesting(const StallReportHandler& handler)
{
    WatchdogState& s = state();
    std::lock_guard<std::mutex> reportLock(s.reportMutex);
    std::lock_guard<std::mutex> k(s.mutex);

    s.handler = handler;
}

std::size_t
AbortWatchdog::getNumWatchedForTesting()
{
    WatchdogState& s = state();
    std::lock_guard<std::mutex> k(s.mutex);

    return s.renders.size();
}

bool
AbortWatchdog::isWatchingForTesting(const AbortableRenderInfoPtr& info)
{
    WatchdogState& s = state();
    std::lock_guard<std::mutex> k(s.mutex);

    for (std::size_t i = 0; i < s.renders.size(); ++i) {
        if (s.renders[i].info.lock() == info) {
            return true;
        }
    }

    return false;
}

NATRON_NAMESPACE_EXIT
