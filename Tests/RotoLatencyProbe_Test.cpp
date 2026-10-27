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
#include <cstdint>
#include <thread>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "Engine/RotoLatencyProbe.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kStages[] = {
    "event_ms", "append_ms", "request_ms", "render_start_ms", "render_end_ms", "upload_ms", "paint_ms"
};
const int kStageCount = 7;

QList<QJsonObject>
readRecords(const QString& path)
{
    QList<QJsonObject> records;
    QFile f(path);

    if (!f.open(QIODevice::ReadOnly)) {
        return records;
    }
    for (const QByteArray& line : f.readAll().split('\n')) {
        if (!line.trimmed().isEmpty()) {
            records.append(QJsonDocument::fromJson(line).object());
        }
    }

    return records;
}

void
sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
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

// The probe is process-wide; a test must not inherit one from the environment or another test.
class RotoLatencyProbeTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        RotoLatencyProbe::stop();
    }

    void TearDown() override
    {
        RotoLatencyProbe::stop();
    }
};
} // namespace

TEST_F(RotoLatencyProbeTest, OffWhenEnvironmentUnsetOrEmpty)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EnvRestore restore("NATRON_ROTO_LATENCY_LOG");

    qunsetenv("NATRON_ROTO_LATENCY_LOG");
    EXPECT_FALSE(RotoLatencyProbe::startFromEnvironment());
    EXPECT_TRUE(RotoLatencyProbe::instance() == nullptr);

    qputenv("NATRON_ROTO_LATENCY_LOG", QByteArray());
    EXPECT_FALSE(RotoLatencyProbe::startFromEnvironment());
    EXPECT_TRUE(RotoLatencyProbe::instance() == nullptr);

    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
}

TEST_F(RotoLatencyProbeTest, OnWhenEnvironmentSet)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EnvRestore restore("NATRON_ROTO_LATENCY_LOG");
    const QString path = dir.filePath(QString::fromUtf8("latency.jsonl"));

    qputenv("NATRON_ROTO_LATENCY_LOG", QFile::encodeName(path));
    EXPECT_TRUE(RotoLatencyProbe::startFromEnvironment());
    EXPECT_TRUE(RotoLatencyProbe::instance() != nullptr);
    // A second start must not replace the running probe.
    EXPECT_FALSE(RotoLatencyProbe::start(path));
    RotoLatencyProbe::stop();
    EXPECT_TRUE(RotoLatencyProbe::instance() == nullptr);
    EXPECT_TRUE(QFile::exists(path));
    EXPECT_TRUE(readRecords(path).isEmpty());
}

TEST_F(RotoLatencyProbeTest, FivePointsGiveFiveOrderedRecords)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QString::fromUtf8("latency.jsonl"));
    ASSERT_TRUE(RotoLatencyProbe::start(path));

    for (int i = 0; i < 5; ++i) {
        RotoLatencyProbe* probe = RotoLatencyProbe::instance();
        ASSERT_TRUE(probe != nullptr);
        probe->noteInputEvent();
        sleepMs(1);
        probe->notePointAppended();
        const std::uint64_t mark = probe->noteRenderRequested();
        EXPECT_EQ(static_cast<std::uint64_t>(i + 1), mark);
        probe->noteRenderStarted(mark);
        probe->noteRenderFinished(mark);
        probe->noteTextureUploaded();
        probe->noteFramePainted();
        sleepMs(1);
    }

    // Each record is written when its frame is painted, before the probe stops.
    const QList<QJsonObject> records = readRecords(path);
    ASSERT_EQ(5, records.size());
    double previousEvent = -1.;
    for (int i = 0; i < records.size(); ++i) {
        const QJsonObject& record = records[i];
        EXPECT_EQ(i + 1, record.value(QString::fromUtf8("seq")).toInt());
        double previous = -1.;
        for (int s = 0; s < kStageCount; ++s) {
            const QJsonValue value = record.value(QString::fromUtf8(kStages[s]));
            ASSERT_TRUE(value.isDouble()) << "record " << i << " stage " << kStages[s];
            EXPECT_GE(value.toDouble(), previous) << "record " << i << " stage " << kStages[s];
            previous = value.toDouble();
        }
        const double event = record.value(QString::fromUtf8("event_ms")).toDouble();
        EXPECT_GT(event, previousEvent);
        previousEvent = event;
    }
}

TEST_F(RotoLatencyProbeTest, RenderCoversOnlyPointsRequestedBeforeIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QString::fromUtf8("latency.jsonl"));
    ASSERT_TRUE(RotoLatencyProbe::start(path));
    RotoLatencyProbe* probe = RotoLatencyProbe::instance();
    ASSERT_TRUE(probe != nullptr);

    probe->noteInputEvent();
    probe->notePointAppended();
    // No event preceded this point: it must not reuse the first point's event.
    probe->notePointAppended();
    const std::uint64_t mark = probe->noteRenderRequested();
    probe->noteInputEvent();
    probe->notePointAppended();
    probe->noteRenderStarted(mark);
    probe->noteRenderFinished(mark);
    probe->noteTextureUploaded();
    probe->noteFramePainted();

    QList<QJsonObject> records = readRecords(path);
    ASSERT_EQ(2, records.size());
    EXPECT_EQ(1, records[0].value(QString::fromUtf8("seq")).toInt());
    EXPECT_EQ(2, records[1].value(QString::fromUtf8("seq")).toInt());
    EXPECT_TRUE(records[0].value(QString::fromUtf8("event_ms")).isDouble());
    EXPECT_TRUE(records[1].value(QString::fromUtf8("event_ms")).isNull());
    EXPECT_EQ(records[0].value(QString::fromUtf8("render_start_ms")).toDouble(),
              records[1].value(QString::fromUtf8("render_start_ms")).toDouble());

    // The unpainted point is flushed on stop with the stages it never reached left null.
    RotoLatencyProbe::stop();
    records = readRecords(path);
    ASSERT_EQ(3, records.size());
    const QJsonObject& late = records[2];
    EXPECT_EQ(3, late.value(QString::fromUtf8("seq")).toInt());
    EXPECT_TRUE(late.value(QString::fromUtf8("event_ms")).isDouble());
    EXPECT_TRUE(late.value(QString::fromUtf8("append_ms")).isDouble());
    EXPECT_TRUE(late.value(QString::fromUtf8("request_ms")).isNull());
    EXPECT_TRUE(late.value(QString::fromUtf8("render_start_ms")).isNull());
    EXPECT_TRUE(late.value(QString::fromUtf8("render_end_ms")).isNull());
    EXPECT_TRUE(late.value(QString::fromUtf8("upload_ms")).isNull());
    EXPECT_TRUE(late.value(QString::fromUtf8("paint_ms")).isNull());
}
