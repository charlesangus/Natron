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

#include "NodeCreationStageLog.h"

#include <chrono>
#include <cstdio>
#include <mutex>

#include <QDebug>
#include <QFile>
#include <QString>

NATRON_NAMESPACE_ENTER

std::atomic<bool> NodeCreationStageLog::s_enabled(false);

namespace {
std::mutex s_mutex;
std::FILE* s_log = nullptr;
} // namespace

double
NodeCreationStageLog::nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool
NodeCreationStageLog::startFromEnvironment()
{
    const QString path = qEnvironmentVariable("NATRON_NODE_CREATION_STAGE_LOG");
    if (path.isEmpty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_log) {
        return false;
    }
    s_log = std::fopen(QFile::encodeName(path).constData(), "a");
    if (!s_log) {
        qWarning() << "NodeCreationStageLog: cannot open" << path << "for appending";

        return false;
    }
    s_enabled.store(true, std::memory_order_relaxed);

    return true;
}

void
NodeCreationStageLog::write(const char* stage,
                            double startMs,
                            double endMs)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_log) {
        return;
    }
    std::fprintf(s_log, "{\"stage\": \"%s\", \"start_ms\": %.3f, \"end_ms\": %.3f}\n", stage, startMs, endMs);
    std::fflush(s_log);
}

NATRON_NAMESPACE_EXIT
