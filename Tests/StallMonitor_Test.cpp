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

#include <chrono>
#include <thread>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "Gui/StallMonitor.h"

NATRON_NAMESPACE_USING

namespace {
void
spinEventLoop(double ms)
{
    const double end = StallMonitor::nowMs() + ms;

    while (StallMonitor::nowMs() < end) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

QList<QByteArray>
readLines(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QList<QByteArray>();
    }
    QList<QByteArray> lines;
    for (const QByteArray& line : f.readAll().split('\n')) {
        if (!line.trimmed().isEmpty()) {
            lines.append(line);
        }
    }

    return lines;
}

class EnvRestore {
public:
    explicit EnvRestore(const char* name)
        : _name(name)
        , _had(qEnvironmentVariableIsSet(name))
        , _value(qgetenv(name))
    {
    }

    ~EnvRestore()
    {
        if (_had) {
            qputenv(_name, _value);
        } else {
            qunsetenv(_name);
        }
    }

private:
    const char* _name;
    bool _had;
    QByteArray _value;
};
} // namespace

TEST(StallMonitor, OffWhenEnvironmentUnset)
{
    EnvRestore restore("NATRON_STALL_LOG");
    qunsetenv("NATRON_STALL_LOG");

    std::unique_ptr<StallMonitor> monitor = StallMonitor::createFromEnvironment();
    EXPECT_TRUE(monitor == nullptr);
    EXPECT_EQ(0, StallMonitor::activeCount());
}

TEST(StallMonitor, OnWhenEnvironmentSet)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EnvRestore restore("NATRON_STALL_LOG");
    qputenv("NATRON_STALL_LOG", QFile::encodeName(dir.filePath(QString::fromUtf8("stalls.jsonl"))));

    {
        std::unique_ptr<StallMonitor> monitor = StallMonitor::createFromEnvironment();
        ASSERT_TRUE(monitor != nullptr);
        EXPECT_EQ(1, StallMonitor::activeCount());
    }
    EXPECT_EQ(0, StallMonitor::activeCount());
}

TEST(StallMonitor, RecordsOneBlockedWindow)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QString::fromUtf8("stalls.jsonl"));

    double blockStart = 0.;
    double blockEnd = 0.;
    {
        std::unique_ptr<StallMonitor> monitor = StallMonitor::create(path, StallMonitor::kDefaultThresholdMs);
        ASSERT_TRUE(monitor != nullptr);
        spinEventLoop(60);

        blockStart = StallMonitor::nowMs();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        blockEnd = StallMonitor::nowMs();

        spinEventLoop(100);
    }

    const QList<QByteArray> lines = readLines(path);
    ASSERT_EQ(1, lines.size());
    const QJsonObject stall = QJsonDocument::fromJson(lines.front()).object();
    ASSERT_TRUE(stall.contains(QString::fromUtf8("start_ms")));
    ASSERT_TRUE(stall.contains(QString::fromUtf8("duration_ms")));
    const double startMs = stall.value(QString::fromUtf8("start_ms")).toDouble();
    const double durationMs = stall.value(QString::fromUtf8("duration_ms")).toDouble();

    EXPECT_GE(durationMs, 180.);
    // start_ms is the last heartbeat before the block, so it may precede it by up to one
    // heartbeat interval plus scheduling slack.
    EXPECT_GE(startMs, blockStart - 3 * StallMonitor::kHeartbeatIntervalMs);
    EXPECT_LE(startMs, blockEnd);
}
