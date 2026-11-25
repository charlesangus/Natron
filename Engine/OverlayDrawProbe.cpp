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

#include "OverlayDrawProbe.h"

#include <chrono>

#include <QDebug>
#include <QFile>

NATRON_NAMESPACE_ENTER

OverlayDrawProbe* OverlayDrawProbe::s_instance = nullptr;

double
OverlayDrawProbe::nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool
OverlayDrawProbe::startFromEnvironment()
{
    const QString path = qEnvironmentVariable("NATRON_OVERLAY_DRAW_LOG");
    if (path.isEmpty()) {
        return false;
    }

    return start(path);
}

bool
OverlayDrawProbe::start(const QString& logPath)
{
    if (instance()) {
        return false;
    }
    std::FILE* log = std::fopen(QFile::encodeName(logPath).constData(), "a");
    if (!log) {
        qWarning() << "OverlayDrawProbe: cannot open" << logPath << "for appending";

        return false;
    }
    s_instance = new OverlayDrawProbe(log);

    return true;
}

void
OverlayDrawProbe::stop()
{
    OverlayDrawProbe* probe = s_instance;
    s_instance = nullptr;

    delete probe;
}

OverlayDrawProbe::OverlayDrawProbe(std::FILE* log)
    : _log(log)
{
}

OverlayDrawProbe::~OverlayDrawProbe()
{
    std::fclose(_log);
}

void
OverlayDrawProbe::notePass(double startMs,
                           double endMs)
{
    std::fprintf(_log, "{\"start_ms\": %.3f, \"end_ms\": %.3f, \"duration_ms\": %.4f}\n", startMs, endMs, endMs - startMs);
    std::fflush(_log);
}

NATRON_NAMESPACE_EXIT
