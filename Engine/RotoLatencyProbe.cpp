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

#include "RotoLatencyProbe.h"

#include <chrono>

#include <QDebug>
#include <QFile>

NATRON_NAMESPACE_ENTER

std::atomic<RotoLatencyProbe*> RotoLatencyProbe::s_instance(nullptr);

namespace {
// steady_clock stamps are positive.
const double kUnset = -1.;

inline bool
isSet(double ms)
{
    return ms >= 0.;
}
} // namespace

double
RotoLatencyProbe::nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool
RotoLatencyProbe::startFromEnvironment()
{
    const QString path = qEnvironmentVariable("NATRON_ROTO_LATENCY_LOG");
    if (path.isEmpty()) {
        return false;
    }

    return start(path);
}

bool
RotoLatencyProbe::start(const QString& logPath)
{
    if (instance()) {
        return false;
    }
    std::FILE* log = std::fopen(QFile::encodeName(logPath).constData(), "a");
    if (!log) {
        qWarning() << "RotoLatencyProbe: cannot open" << logPath << "for appending";

        return false;
    }
    s_instance.store(new RotoLatencyProbe(log), std::memory_order_release);

    return true;
}

void
RotoLatencyProbe::stop()
{
    RotoLatencyProbe* probe = s_instance.exchange(nullptr, std::memory_order_acq_rel);

    delete probe;
}

RotoLatencyProbe::RotoLatencyProbe(std::FILE* log)
    : _log(log)
    , _mutex()
    , _pending()
    , _lastSeq(0)
    , _pendingEventMs(kUnset)
{
}

RotoLatencyProbe::~RotoLatencyProbe()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const PointRecord& record : _pending) {
            writeRecord(record);
        }
        _pending.clear();
    }
    std::fclose(_log);
}

void
RotoLatencyProbe::noteInputEvent()
{
    const double now = nowMs();
    std::lock_guard<std::mutex> lock(_mutex);

    _pendingEventMs = now;
}

void
RotoLatencyProbe::notePointAppended()
{
    const double now = nowMs();
    std::lock_guard<std::mutex> lock(_mutex);
    PointRecord record;

    record.seq = ++_lastSeq;
    for (int i = 0; i < eStageCount; ++i) {
        record.stamps[i] = kUnset;
    }
    // An event is consumed by the point it produced, so a point appended outside any event
    // (from a script) does not inherit a stale one.
    record.stamps[eStageEvent] = _pendingEventMs;
    _pendingEventMs = kUnset;
    record.stamps[eStageAppend] = now;
    _pending.push_back(record);
}

std::uint64_t
RotoLatencyProbe::noteRenderRequested()
{
    const double now = nowMs();
    std::lock_guard<std::mutex> lock(_mutex);

    for (PointRecord& record : _pending) {
        if (!isSet(record.stamps[eStageRequest])) {
            record.stamps[eStageRequest] = now;
        }
    }

    return _lastSeq;
}

void
RotoLatencyProbe::noteRenderStarted(std::uint64_t mark)
{
    const double now = nowMs();
    std::lock_guard<std::mutex> lock(_mutex);

    for (PointRecord& record : _pending) {
        if (record.seq > mark) {
            break;
        }
        if (!isSet(record.stamps[eStageRenderStart])) {
            record.stamps[eStageRenderStart] = now;
        }
    }
}

void
RotoLatencyProbe::noteRenderFinished(std::uint64_t mark)
{
    const double now = nowMs();
    std::lock_guard<std::mutex> lock(_mutex);

    for (PointRecord& record : _pending) {
        if (record.seq > mark) {
            break;
        }
        if (isSet(record.stamps[eStageRenderStart]) && !isSet(record.stamps[eStageRenderEnd])) {
            record.stamps[eStageRenderEnd] = now;
        }
    }
}

void
RotoLatencyProbe::noteTextureUploaded()
{
    const double now = nowMs();
    std::lock_guard<std::mutex> lock(_mutex);

    for (PointRecord& record : _pending) {
        if (isSet(record.stamps[eStageRenderEnd]) && !isSet(record.stamps[eStageUpload])) {
            record.stamps[eStageUpload] = now;
        }
    }
}

void
RotoLatencyProbe::noteFramePainted()
{
    const double now = nowMs();
    std::lock_guard<std::mutex> lock(_mutex);
    std::deque<PointRecord> waiting;

    for (PointRecord& record : _pending) {
        if (isSet(record.stamps[eStageUpload])) {
            record.stamps[eStagePaint] = now;
            writeRecord(record);
        } else {
            waiting.push_back(record);
        }
    }
    _pending.swap(waiting);
}

void
RotoLatencyProbe::writeRecord(const PointRecord& record)
{
    static const char* const names[eStageCount] = {
        "event_ms", "append_ms", "request_ms", "render_start_ms", "render_end_ms", "upload_ms", "paint_ms"
    };

    std::fprintf(_log, "{\"seq\": %llu", static_cast<unsigned long long>(record.seq));
    for (int i = 0; i < eStageCount; ++i) {
        if (isSet(record.stamps[i])) {
            std::fprintf(_log, ", \"%s\": %.3f", names[i], record.stamps[i]);
        } else {
            std::fprintf(_log, ", \"%s\": null", names[i]);
        }
    }
    std::fprintf(_log, "}\n");
    std::fflush(_log);
}

std::atomic<RotoRenderProfile*> RotoRenderProfile::s_instance(nullptr);
thread_local RotoRenderProfile::Record* RotoRenderProfile::t_current = nullptr;

namespace {
const char* const kProfileStageNames[RotoRenderProfile::eStageCount] = {
    "cache_lookup", "allocate", "polygon", "feather_setup", "fill", "feather", "conversion", "setup", "tree"
};

bool
stageBelongsTo(RotoRenderProfile::KindEnum kind,
               int stage)
{
    const bool rotoPaintStage = (stage == RotoRenderProfile::eStageSetup) || (stage == RotoRenderProfile::eStageTree);

    return (kind == RotoRenderProfile::eKindRotoPaint) == rotoPaintStage;
}

void
writeJsonString(std::FILE* log,
                const std::string& text)
{
    std::fputc('"', log);
    for (char c : text) {
        if ((c == '"') || (c == '\\')) {
            std::fputc('\\', log);
            std::fputc(c, log);
        } else if ((unsigned char)c < 0x20) {
            std::fprintf(log, "\\u%04x", (unsigned int)(unsigned char)c);
        } else {
            std::fputc(c, log);
        }
    }
    std::fputc('"', log);
}

// Charges the time since the record's last stamp to the stage running then, or to no stage.
void
charge(RotoRenderProfile::Record* record,
       double now)
{
    const double elapsed = now - record->lastMs;

    if (record->current >= 0) {
        record->stageMs[record->current] += elapsed;
    } else {
        record->otherMs += elapsed;
    }
    record->lastMs = now;
}
} // namespace

RotoRenderProfile::Scope::Scope(KindEnum kind,
                                double time,
                                unsigned int mipmap)
    : _profile(RotoRenderProfile::instance())
    , _record(nullptr)
    , _previous(nullptr)
    , _storage()
{
    if (!_profile) {
        return;
    }
    _storage.emplace();
    Record& record = *_storage;
    record.kind = kind;
    record.time = time;
    record.mipmap = mipmap;
    record.current = -1;
    record.startMs = RotoLatencyProbe::nowMs();
    record.lastMs = record.startMs;
    _record = &record;
    _previous = t_current;
    t_current = _record;
}

RotoRenderProfile::Scope::~Scope()
{
    if (!_record) {
        return;
    }
    charge(_record, RotoLatencyProbe::nowMs());
    t_current = _previous;
    _profile->writeRecord(*_record);
}

void
RotoRenderProfile::Scope::setNames(const std::string& node,
                                   const std::string& item,
                                   const std::string& type)
{
    if (_record) {
        _record->node = node;
        _record->item = item;
        _record->type = type;
    }
}

void
RotoRenderProfile::Scope::setHit(bool hit)
{
    if (_record) {
        _record->hit = hit;
    }
}

void
RotoRenderProfile::Scope::setPixels(long long pixels)
{
    if (_record) {
        _record->pixels = pixels;
    }
}

void
RotoRenderProfile::Scope::setItems(int items)
{
    if (_record) {
        _record->items = items;
    }
}

void
RotoRenderProfile::StageTimer::begin(StageEnum stage)
{
    charge(_record, RotoLatencyProbe::nowMs());
    _previous = _record->current;
    _record->current = stage;
    ++_record->stageCalls[stage];
}

void
RotoRenderProfile::StageTimer::end()
{
    charge(_record, RotoLatencyProbe::nowMs());
    _record->current = _previous;
}

bool
RotoRenderProfile::startFromEnvironment()
{
    const QString path = qEnvironmentVariable("NATRON_ROTO_RENDER_PROFILE");
    if (path.isEmpty()) {
        return false;
    }

    return start(path);
}

bool
RotoRenderProfile::start(const QString& logPath)
{
    if (instance()) {
        return false;
    }
    std::FILE* log = std::fopen(QFile::encodeName(logPath).constData(), "a");
    if (!log) {
        qWarning() << "RotoRenderProfile: cannot open" << logPath << "for appending";

        return false;
    }
    s_instance.store(new RotoRenderProfile(log), std::memory_order_release);

    return true;
}

void
RotoRenderProfile::stop()
{
    RotoRenderProfile* profile = s_instance.exchange(nullptr, std::memory_order_acq_rel);

    delete profile;
}

RotoRenderProfile::RotoRenderProfile(std::FILE* log)
    : _log(log)
    , _mutex()
{
}

RotoRenderProfile::~RotoRenderProfile()
{
    std::fclose(_log);
}

void
RotoRenderProfile::writeRecord(const Record& record)
{
    const double total = record.lastMs - record.startMs;
    std::lock_guard<std::mutex> lock(_mutex);

    if (record.kind == eKindMask) {
        std::fprintf(_log, "{\"kind\": \"mask\", \"node\": ");
        writeJsonString(_log, record.node);
        std::fprintf(_log, ", \"item\": ");
        writeJsonString(_log, record.item);
        std::fprintf(_log, ", \"type\": ");
        writeJsonString(_log, record.type);
        std::fprintf(_log, ", \"time\": %g, \"mipmap\": %u, \"hit\": %s, \"pixels\": %lld",
                     record.time, record.mipmap, record.hit ? "true" : "false", record.pixels);
    } else {
        std::fprintf(_log, "{\"kind\": \"rotopaint\", \"node\": ");
        writeJsonString(_log, record.node);
        std::fprintf(_log, ", \"time\": %g, \"mipmap\": %u, \"items\": %d, \"pixels\": %lld",
                     record.time, record.mipmap, record.items, record.pixels);
    }
    std::fprintf(_log, ", \"total_ms\": %.4f, \"other_ms\": %.4f", total, record.otherMs);
    for (int i = 0; i < eStageCount; ++i) {
        if (stageBelongsTo(record.kind, i)) {
            std::fprintf(_log, ", \"%s_ms\": %.4f, \"%s_calls\": %d",
                         kProfileStageNames[i], record.stageMs[i], kProfileStageNames[i], record.stageCalls[i]);
        }
    }
    std::fprintf(_log, "}\n");
    std::fflush(_log);
}

NATRON_NAMESPACE_EXIT
