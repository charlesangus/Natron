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
#include <mutex>
#include <sstream>
#include <vector>

#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

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

struct WatchdogState {
    std::mutex mutex;
    std::vector<WatchedRender> renders;
    // Renders that were destroyed are only pruned by a scan, which never runs while the main
    // thread is blocked; pruning on insertion past this size keeps the list bounded meanwhile.
    std::size_t pruneThreshold = 64;
    // Set once a timer start was queued, cleared by the scan that stops the timer.
    bool timerWanted = false;
    AbortWatchdog::StallReportHandler handler;

    // Main thread only.
    QPointer<QTimer> timer;
    bool reportOpen = false;
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

void scan();

void
startTimerOnMainThread()
{
    WatchdogState& s = state();

    if (!s.timer) {
        s.timer = new QTimer(QCoreApplication::instance());
        QObject::connect(s.timer.data(), &QTimer::timeout, s.timer.data(), []() { scan(); });
    }
    if (!s.timer->isActive()) {
        s.timer->start(scanIntervalMs());
    }
}

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
    if (appPTR->isBackground()) {
        qDebug() << ss.str().c_str();

        return;
    }

    ss << AbortWatchdog::tr("Would you like to kill these renders?").toStdString() << std::endl
       << std::endl;
    ss << AbortWatchdog::tr("WARNING: Killing them may not work or may leave %1 in a bad state. The application may crash or freeze as a consequence of this. It is advised to restart %1 instead.").arg(QString::fromUtf8(NATRON_APPLICATION_NAME)).toStdString();

    s.reportOpen = true;
    StandardButtonEnum reply = Dialogs::questionDialog(AbortWatchdog::tr("A Render is not responding anymore").toStdString(), ss.str(), false, StandardButtons(eStandardButtonYes | eStandardButtonNo), eStandardButtonNo);
    s.reportOpen = false;
    if (reply == eStandardButtonYes) {
        for (std::size_t i = 0; i < stalled.size(); ++i) {
            stalled[i]->killRegisteredThreads();
        }
    }
} // report

void
scan()
{
    WatchdogState& s = state();

    // The question dialog runs a nested event loop; renders keep waiting until it closes.
    if (s.reportOpen) {
        return;
    }

    const Clock::time_point now = Clock::now();
    const Clock::duration timeout = std::chrono::milliseconds(AbortWatchdog::getTimeoutMs());
    std::vector<AbortableRenderInfoPtr> due;
    bool stop = false;
    {
        std::lock_guard<std::mutex> k(s.mutex);
        std::vector<WatchedRender>::iterator kept = s.renders.begin();
        for (std::vector<WatchedRender>::iterator it = s.renders.begin(); it != s.renders.end(); ++it) {
            AbortableRenderInfoPtr info = it->info.lock();
            if (!info) {
                continue;
            }
            if (now - it->abortedAt < timeout) {
                *kept++ = *it;
                continue;
            }
            due.push_back(info);
        }
        s.renders.erase(kept, s.renders.end());
        if (s.renders.empty()) {
            s.timerWanted = false;
            stop = true;
        }
    }
    if (stop) {
        s.timer->stop();
    } else if (s.timer->interval() != scanIntervalMs()) {
        s.timer->setInterval(scanIntervalMs());
    }

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
} // scan

NATRON_NAMESPACE_ANONYMOUS_EXIT

void
AbortWatchdog::watch(const AbortableRenderInfoWPtr& info)
{
    QCoreApplication* app = QCoreApplication::instance();

    if (!app) {
        return;
    }

    WatchdogState& s = state();
    bool startTimer = false;
    {
        std::lock_guard<std::mutex> k(s.mutex);
        if (s.renders.size() >= s.pruneThreshold) {
            s.renders.erase(std::remove_if(s.renders.begin(), s.renders.end(), [](const WatchedRender& r) { return r.info.expired(); }), s.renders.end());
            s.pruneThreshold = std::max<std::size_t>(64, s.renders.size() * 2);
        }
        WatchedRender render;
        render.info = info;
        render.abortedAt = Clock::now();
        s.renders.push_back(render);
        if (!s.timerWanted) {
            s.timerWanted = true;
            startTimer = true;
        }
    }
    if (startTimer) {
        QMetaObject::invokeMethod(app, []() { startTimerOnMainThread(); }, Qt::QueuedConnection);
    }
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
