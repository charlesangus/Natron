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

#include "Gui/SceneDrawTiming.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <QtCore/QString>
#include <QtCore/QVariantMap>

#ifdef emit
#define NATRON_SCENEDRAWTIMING_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/arch/timing.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/trace/collector.h>
#include <pxr/base/trace/eventNode.h>
#include <pxr/base/trace/reporter.h>
#include <pxr/base/trace/reporterDataSourceCollector.h>
#include <pxr/base/trace/threads.h>
#include <pxr/imaging/hd/perfLog.h>
#ifdef NATRON_SCENEDRAWTIMING_RESTORE_EMIT
#undef NATRON_SCENEDRAWTIMING_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// TraceCollector and HdPerfLog are process-wide: one viewer traces at a time.
std::atomic<bool> traceHeld { false };

const std::size_t kKeptRecords = 256;

struct Stage {
    const char* name;
    const char* scope;
};

// Scope names as TRACE_FUNCTION records them in the pinned USD build's libraries. A scope nested in
// one of the same stage is not counted again.
const Stage kStages[] = {
    { "adapterAdded", "HdSceneIndexAdapterSceneDelegate::PrimsAdded" },
    { "adapterRemoved", "HdSceneIndexAdapterSceneDelegate::PrimsRemoved" },
    { "adapterDirtied", "HdSceneIndexAdapterSceneDelegate::PrimsDirtied" },
    { "insertRprim", "HdRenderIndex::InsertRprim" },
    { "removeRprim", "HdRenderIndex::RemoveRprim" },
    { "prepareBatch", "UsdImagingGLEngine::PrepareBatch" },
    { "renderBatch", "UsdImagingGLEngine::RenderBatch" },
    { "hdExecute", "HdEngine::Execute" },
    { "sync", "HdRenderIndex::SyncAll" },
    { "commitResources", "HdStRenderDelegate::CommitResources" },
    { "renderTaskExecute", "HdxRenderTask::Execute" },
    { "renderPassExecute", "HdSt_RenderPass::_Execute" },
    { "regather", "HdSt_RenderPass::_UpdateDrawItems" },
    { "getDrawItems", "HdSt_DrawItemsCache::GetDrawItems" },
    { "getDrawItems", "HdRenderIndex::GetDrawItems" },
    { "commandBuffer", "HdSt_RenderPass::_UpdateCommandBuffer" },
    { "batchRebuild", "HdStCommandBuffer::_RebuildDrawBatches" },
    { "visibilitySync", "HdStCommandBuffer::SyncDrawItemVisibility" },
    { "frustumCull", "HdStCommandBuffer::_FrustumCullCPU" },
    { "prepareDraw", "HdStCommandBuffer::PrepareDraw" },
    { "executeDraw", "HdStCommandBuffer::ExecuteDraw" },
};
const std::size_t kStageCount = sizeof(kStages) / sizeof(kStages[0]);

// Counter names of HdPerfTokens and HdStPerfTokens; by name, so this library need not link hdSt.
const char* const kCounters[] = {
    "drawItemsFetched",
    "drawItemsCacheHit",
    "drawItemsCacheMiss",
    "drawItemsCacheStale",
    "rebuildBatches",
    "drawBatches",
    "drawCalls",
    "dirtyLists",
    "dirtyListsRebuilt",
    "skipInvisibleRprimSync",
    "bufferSourcesResolved",
    "garbageCollected",
};

double
msBetween(SceneDrawTiming::Clock::time_point from,
          SceneDrawTiming::Clock::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// A key holds the scope where the scope's name ends it or is followed by its argument list, so
// _Execute does not match _ExecuteDraw.
bool
holdsScope(const std::string& key,
           const char* scope)
{
    const std::size_t length = std::strlen(scope);
    for (std::size_t at = key.find(scope); at != std::string::npos; at = key.find(scope, at + 1)) {
        const std::size_t end = at + length;
        if ((end == key.size()) || (!std::isalnum(static_cast<unsigned char>(key[end])) && (key[end] != '_'))) {
            return true;
        }
    }

    return false;
}

std::map<std::string, double>
readCounters()
{
    std::map<std::string, double> values;
    HdPerfLog& log = HdPerfLog::GetInstance();
    for (const char* name : kCounters) {
        values[name] = log.GetCounter(TfToken(name));
    }

    return values;
}

QVariantMap
toVariantMap(const std::map<std::string, double>& values)
{
    QVariantMap map;
    for (const auto& value : values) {
        map[QString::fromStdString(value.first)] = value.second;
    }

    return map;
}
} // namespace

struct SceneDrawTiming::Record {
    unsigned long long firstPosted = 0;
    unsigned long long lastPosted = 0;
    int applies = 0;
    bool revealStep = false;
    Clock::time_point startedAt;
    Clock::time_point firstApplyStart;
    double applyMs = 0.;
    unsigned long long hiddenSent = 0;
    unsigned long long shownSent = 0;
    Clock::time_point renderStart;
    Clock::time_point renderEnd;
    int renderPasses = 0;
    double gpuTailMs = -1.;
    bool onPaint = false;
    bool frameDone = false;
    Clock::time_point frameDoneAt;
    Clock::time_point presentedAt;
    bool traced = false;
    unsigned long long traceId = 0;
    bool tracePending = false;
    std::string traceSkipped;
    bool traceThreadFound = false;
    double traceCollectMs = 0.;
    std::map<std::string, double> stagesMs;
    std::map<std::string, double> stageCounts;
    std::map<std::string, double> counterDeltas;
    std::map<std::string, double> counterValues;
};

bool
SceneDrawTiming::enabledByEnvironment()
{
    const char* value = std::getenv("NATRON_DRAW_STATE_TIMING");

    return value && (std::strcmp(value, "1") == 0);
}

struct SceneDrawTiming::TraceResult {
    bool threadFound = false;
    std::map<std::string, double> stagesMs;
    std::map<std::string, double> stageCounts;
};

SceneDrawTiming::SceneDrawTiming() = default;

SceneDrawTiming::~SceneDrawTiming()
{
    if (_reader.joinable()) {
        _reader.join();
    }
    if (_tracing) {
        TraceCollector::GetInstance().SetEnabled(false);
        HdPerfLog::GetInstance().Disable();
        traceHeld = false;
    }
}

void
SceneDrawTiming::setTraceNext(bool trace)
{
    _traceNext = trace;
}

void
SceneDrawTiming::setTraceReadCallback(const std::function<void()>& callback)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _traceRead = callback;
}

void
SceneDrawTiming::startTrace()
{
    if (_reader.joinable()) {
        _reader.join();
    }
    _countersAtStart = readCounters();
    HdPerfLog::GetInstance().Enable();
    TraceCollector& collector = TraceCollector::GetInstance();
    collector.Clear();
    collector.SetEnabled(true);
    _tracing = true;
}

void
SceneDrawTiming::applyStarting()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_open || _tracing || _reading || !_traceNext) {
        return;
    }
    bool expected = false;
    if (traceHeld.compare_exchange_strong(expected, true)) {
        startTrace();
    }
}

void
SceneDrawTiming::applied(unsigned long long firstPosted,
                         unsigned long long lastPosted,
                         Clock::time_point startedAt,
                         Clock::time_point applyStart,
                         Clock::time_point applyEnd,
                         unsigned long long hiddenSent,
                         unsigned long long shownSent,
                         bool revealStep)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_open) {
        _open.reset(new Record);
        _open->firstPosted = firstPosted;
        _open->startedAt = startedAt;
        _open->firstApplyStart = applyStart;
        _open->traced = _tracing;
        if (_traceNext && !_tracing) {
            _open->traceSkipped = _reading ? "the last trace is still being read" : "another viewer holds the trace";
        }
    }
    Record& record = *_open;
    if (startedAt < record.startedAt) {
        record.startedAt = startedAt;
    }
    // A reveal step alone carries no new change: its first is past its last.
    if ((record.firstPosted > record.lastPosted) && (firstPosted <= lastPosted)) {
        record.firstPosted = firstPosted;
    }
    record.lastPosted = (std::max)(record.lastPosted, lastPosted);
    ++record.applies;
    record.revealStep = record.revealStep || revealStep;
    record.applyMs += msBetween(applyStart, applyEnd);
    record.hiddenSent += hiddenSent;
    record.shownSent += shownSent;
}

bool
SceneDrawTiming::frameStarting(Clock::time_point renderStart)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_open) {
        return false;
    }
    _open->renderStart = renderStart;

    return true;
}

// Render thread, with the mutex held.
void
SceneDrawTiming::stopTrace(Record* record)
{
    TraceCollector::GetInstance().SetEnabled(false);
    record->counterValues = readCounters();
    HdPerfLog::GetInstance().Disable();
    for (const auto& value : record->counterValues) {
        record->counterDeltas[value.first] = value.second - _countersAtStart[value.first];
    }
    _tracing = false;
    _reading = true;
    record->traceId = ++_lastTraceId;
    record->tracePending = true;
}

// Reading a trace takes seconds on a large scene, which no record may count as its own.
void
SceneDrawTiming::readTrace(unsigned long long traceId,
                           const std::string& thread)
{
    const Clock::time_point start = Clock::now();
    TraceCollector& collector = TraceCollector::GetInstance();
    TraceResult result;
    // The source listens for the collection the collector announces, so it has to exist first.
    TraceReporterDataSourceCollector::ThisRefPtr source = TraceReporterDataSourceCollector::New();
    collector.CreateCollection();
    TraceReporterRefPtr reporter = TraceReporter::New("Natron draw state timing", std::move(source));
    reporter->UpdateTraceTrees();
    TraceEventNodeRefPtr root = reporter->GetEventRoot();

    // The render thread's own scopes, so that time spent in parallel workers is not added up.
    TraceEventNodeRefPtr walked = root;
    if (root) {
        for (const TraceEventNodeRefPtr& child : root->GetChildrenRef()) {
            if (child && (child->GetKey().GetString() == thread)) {
                walked = child;
                result.threadFound = true;
                break;
            }
        }
    }
    std::vector<std::string> stageNames;
    for (const Stage& stage : kStages) {
        if (std::find(stageNames.begin(), stageNames.end(), stage.name) == stageNames.end()) {
            stageNames.push_back(stage.name);
            result.stagesMs[stage.name] = 0.;
            result.stageCounts[stage.name] = 0.;
        }
    }
    struct Pending {
        TraceEventNodeRefPtr node;
        std::vector<bool> inside;
    };
    std::vector<Pending> pending;
    if (walked) {
        pending.push_back(Pending { walked, std::vector<bool>(stageNames.size(), false) });
    }
    while (!pending.empty()) {
        Pending current = std::move(pending.back());
        pending.pop_back();
        const std::string& key = current.node->GetKey().GetString();
        std::vector<bool> inside = current.inside;
        for (std::size_t i = 0; i < kStageCount; ++i) {
            const std::size_t slot = std::size_t(std::find(stageNames.begin(), stageNames.end(), kStages[i].name) - stageNames.begin());
            if (!inside[slot] && holdsScope(key, kStages[i].scope)) {
                const TraceEvent::TimeStamp begin = current.node->GetBeginTime();
                const TraceEvent::TimeStamp end = current.node->GetEndTime();
                result.stagesMs[kStages[i].name] += (end > begin) ? (double(ArchTicksToNanoseconds(end - begin)) / 1e6) : 0.;
                result.stageCounts[kStages[i].name] += 1.;
                inside[slot] = true;
            }
        }
        for (const TraceEventNodeRefPtr& child : current.node->GetChildrenRef()) {
            if (child) {
                pending.push_back(Pending { child, inside });
            }
        }
    }
    collector.Clear();
    const double collectMs = msBetween(start, Clock::now());
    std::function<void()> traceRead;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (std::deque<std::unique_ptr<Record>>* records : { &_rendered, &_completed }) {
            for (const std::unique_ptr<Record>& record : *records) {
                if (record->traceId == traceId) {
                    record->tracePending = false;
                    record->traceThreadFound = result.threadFound;
                    record->traceCollectMs = collectMs;
                    record->stagesMs = std::move(result.stagesMs);
                    record->stageCounts = std::move(result.stageCounts);
                }
            }
        }
        _reading = false;
        traceRead = _traceRead;
    }
    traceHeld = false;
    if (traceRead) {
        traceRead();
    }
}

void
SceneDrawTiming::frameEnded(Clock::time_point renderEnd,
                            int renderPasses,
                            double gpuTailMs,
                            bool onPaint)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::unique_ptr<Record> record = std::move(_open);
    if (!record) {
        return;
    }
    record->renderEnd = renderEnd;
    record->renderPasses = renderPasses;
    record->gpuTailMs = gpuTailMs;
    record->onPaint = onPaint;
    if (record->traced && _tracing) {
        stopTrace(record.get());
        if (_reader.joinable()) {
            _reader.join();
        }
        // The trace keys each thread's events by its id, so the render thread's is taken here.
        _reader = std::thread(&SceneDrawTiming::readTrace, this, record->traceId, TraceThreadId().ToString());
    }
    _rendered.push_back(std::move(record));
}

void
SceneDrawTiming::frameDone(Clock::time_point at)
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (const std::unique_ptr<Record>& record : _rendered) {
        if (!record->frameDone && !record->onPaint) {
            record->frameDone = true;
            record->frameDoneAt = at;
        }
    }
}

bool
SceneDrawTiming::presented(unsigned long long absorbed,
                           Clock::time_point at)
{
    std::lock_guard<std::mutex> lock(_mutex);
    bool completed = false;
    while (!_rendered.empty() && (_rendered.front()->lastPosted <= absorbed)) {
        _rendered.front()->presentedAt = at;
        _completed.push_back(std::move(_rendered.front()));
        _rendered.pop_front();
        completed = true;
    }
    while (_completed.size() > kKeptRecords) {
        _completed.pop_front();
    }

    return completed;
}

QVariantList
SceneDrawTiming::records() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    QVariantList list;
    for (const std::unique_ptr<Record>& record : _completed) {
        QVariantMap map;
        map[QString::fromUtf8("firstPosted")] = QVariant::fromValue<qulonglong>(record->firstPosted);
        map[QString::fromUtf8("lastPosted")] = QVariant::fromValue<qulonglong>(record->lastPosted);
        map[QString::fromUtf8("applies")] = record->applies;
        map[QString::fromUtf8("revealStep")] = record->revealStep;
        map[QString::fromUtf8("queueMs")] = msBetween(record->startedAt, record->firstApplyStart);
        map[QString::fromUtf8("applyMs")] = record->applyMs;
        map[QString::fromUtf8("hiddenSent")] = QVariant::fromValue<qulonglong>(record->hiddenSent);
        map[QString::fromUtf8("shownSent")] = QVariant::fromValue<qulonglong>(record->shownSent);
        map[QString::fromUtf8("renderStartMs")] = msBetween(record->startedAt, record->renderStart);
        map[QString::fromUtf8("renderMs")] = msBetween(record->renderStart, record->renderEnd);
        map[QString::fromUtf8("renderPasses")] = record->renderPasses;
        map[QString::fromUtf8("gpuTailMs")] = (record->gpuTailMs < 0.) ? QVariant() : QVariant(record->gpuTailMs);
        map[QString::fromUtf8("onPaint")] = record->onPaint;
        map[QString::fromUtf8("frameDoneMs")] = record->frameDone ? QVariant(msBetween(record->startedAt, record->frameDoneAt)) : QVariant();
        map[QString::fromUtf8("presentedMs")] = msBetween(record->startedAt, record->presentedAt);
        map[QString::fromUtf8("traced")] = record->traced;
        if (!record->traceSkipped.empty()) {
            map[QString::fromUtf8("traceSkipped")] = QString::fromStdString(record->traceSkipped);
        }
        if (record->traced) {
            map[QString::fromUtf8("tracePending")] = record->tracePending;
            map[QString::fromUtf8("traceThreadFound")] = record->traceThreadFound;
            map[QString::fromUtf8("traceCollectMs")] = record->traceCollectMs;
            map[QString::fromUtf8("stagesMs")] = toVariantMap(record->stagesMs);
            map[QString::fromUtf8("stageCounts")] = toVariantMap(record->stageCounts);
            map[QString::fromUtf8("counterDeltas")] = toVariantMap(record->counterDeltas);
            map[QString::fromUtf8("counterValues")] = toVariantMap(record->counterValues);
        }
        list.push_back(map);
    }

    return list;
}

NATRON_NAMESPACE_EXIT
