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

#include "StallMonitor.h"

#include <chrono>

#include <QDebug>
#include <QFile>

NATRON_NAMESPACE_ENTER

std::atomic<int> StallMonitor::s_active(0);

double
StallMonitor::nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::unique_ptr<StallMonitor>
StallMonitor::createFromEnvironment()
{
    const QString path = qEnvironmentVariable("NATRON_STALL_LOG");
    if (path.isEmpty()) {
        return std::unique_ptr<StallMonitor>();
    }
    int thresholdMs = kDefaultThresholdMs;
    bool ok = false;
    const int envThreshold = qEnvironmentVariableIntValue("NATRON_STALL_THRESHOLD_MS", &ok);
    if (ok && envThreshold > 0) {
        thresholdMs = envThreshold;
    }

    return create(path, thresholdMs);
}

std::unique_ptr<StallMonitor>
StallMonitor::create(const QString& logPath,
                     int thresholdMs)
{
    std::FILE* log = std::fopen(QFile::encodeName(logPath).constData(), "a");
    if (!log) {
        qWarning() << "StallMonitor: cannot open" << logPath << "for appending";

        return std::unique_ptr<StallMonitor>();
    }

    return std::unique_ptr<StallMonitor>(new StallMonitor(log, thresholdMs));
}

StallMonitor::StallMonitor(std::FILE* log,
                           int thresholdMs)
    : _log(log)
    , _thresholdMs(thresholdMs)
    , _heartbeatTimer()
    , _lastBeatMs(nowMs())
    , _stopMutex()
    , _stopCond()
    , _stop(false)
    , _watcher()
{
    _heartbeatTimer.setTimerType(Qt::PreciseTimer);
    _heartbeatTimer.setInterval(kHeartbeatIntervalMs);
    QObject::connect(&_heartbeatTimer, &QTimer::timeout, [this]() {
        _lastBeatMs.store(nowMs());
    });
    _heartbeatTimer.start();
    ++s_active;
    _watcher = std::thread(&StallMonitor::watch, this);
}

StallMonitor::~StallMonitor()
{
    _heartbeatTimer.stop();
    {
        std::lock_guard<std::mutex> lock(_stopMutex);
        _stop = true;
    }
    _stopCond.notify_all();
    _watcher.join();
    --s_active;
    std::fclose(_log);
}

int
StallMonitor::activeCount()
{
    return s_active.load();
}

void
StallMonitor::watch()
{
    bool inStall = false;
    double stallStartMs = 0.;
    std::unique_lock<std::mutex> lock(_stopMutex);

    while (!_stop) {
        // Polling at half the heartbeat interval bounds detection latency without burning a core.
        _stopCond.wait_for(lock, std::chrono::milliseconds(kHeartbeatIntervalMs / 2));
        if (_stop) {
            break;
        }
        const double beat = _lastBeatMs.load();
        if (!inStall) {
            if (nowMs() - beat > _thresholdMs) {
                inStall = true;
                stallStartMs = beat;
            }
        } else if (beat > stallStartMs) {
            writeStall(stallStartMs, beat - stallStartMs);
            inStall = false;
        }
    }
    // A stall still open at shutdown is the main thread tearing down, not a hang worth reporting.
}

void
StallMonitor::writeStall(double startMs,
                         double durationMs)
{
    std::fprintf(_log, "{\"start_ms\": %.3f, \"duration_ms\": %.3f}\n", startMs, durationMs);
    std::fflush(_log);
}

NATRON_NAMESPACE_EXIT
