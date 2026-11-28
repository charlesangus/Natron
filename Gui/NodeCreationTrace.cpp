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

#include "NodeCreationTrace.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QEvent>
#include <QMetaEnum>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/NodeCreationStageLog.h"

NATRON_NAMESPACE_ENTER

std::atomic<bool> NodeCreationTrace::s_enabled(false);

namespace {
const char* const kPrefix = "NodeCreationTrace: ";
const double kGapMs = 4.;
const double kRecordedEventMs = 0.3;
const double kActivityMs = 2.;
const double kQuietMs = 300.;
const double kMinSessionMs = 500.;
const double kMaxSessionMs = 5000.;
const int kWatchdogMs = 50;
const std::size_t kMaxStages = 50000;
const std::size_t kMaxEvents = 20000;
const std::size_t kMaxTimelineLines = 400;

struct Frame {
    double start = 0.;
    // Time of the events dispatched inside this one and of the event-loop waits inside it.
    double nested = 0.;
    QEvent::Type type = QEvent::None;
    const char* className = "";
    const char* ownerClass = nullptr;
    QString objectName;
    bool own = false;
};

struct EventRecord {
    double start;
    double end;
    double self;
    QEvent::Type type;
    const char* className;
    const char* ownerClass;
    QString objectName;
};

struct StageRecord {
    std::string name;
    double start;
    double end;
};

struct Interval {
    double start;
    double end;
};

struct Session {
    std::string label;
    std::string input;
    double t0 = 0.;
    double choiceMs = 0.;
    double lastActivityMs = 0.;
    double panelPaintMs = -1.;
    double panelShownMs = -1.;
    // The stack depth at which the event holding the panel's first paint returns, -1 while none.
    int panelFrameDepth = -1;
    bool panelExpected = false;
    std::vector<StageRecord> stages;
    std::vector<EventRecord> events;
    std::vector<Interval> idle;
    int smallEvents = 0;
    double smallEventsMs = 0.;
    std::map<std::string, std::pair<int, double>> offMain;
};

QThread* s_mainThread = nullptr;
std::vector<Frame> s_stack;
double s_blockStart = -1.;
std::unique_ptr<Session> s_session;
std::atomic<bool> s_sessionActive(false);
std::mutex s_offMainMutex;
QTimer* s_watchdog = nullptr;
std::vector<QString> s_pendingReports;

bool
isInputEvent(QEvent::Type type)
{
    switch (type) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::ShortcutOverride:
    case QEvent::Shortcut:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
    case QEvent::TouchEnd:
        return true;
    default:
        return false;
    }
}

std::string
eventTypeName(QEvent::Type type)
{
    const char* key = QMetaEnum::fromType<QEvent::Type>().valueToKey(type);

    return key ? std::string(key) : std::to_string(static_cast<int>(type));
}

std::string
describe(QEvent::Type type,
         const char* className,
         const char* ownerClass,
         const QString& objectName)
{
    std::string s = eventTypeName(type);
    s += " -> ";
    s += className;
    if (ownerClass) {
        s += '<';
        s += ownerClass;
        s += '>';
    }
    if (!objectName.isEmpty()) {
        s += " \"";
        s += objectName.toStdString();
        s += '"';
    }

    return s;
}

std::string
format(const char* fmt,
       double a = 0.,
       double b = 0.,
       double c = 0.,
       double d = 0.)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);

    return std::string(buf);
}

void
endIdle(double now)
{
    if (s_blockStart < 0.) {
        return;
    }
    const double idle = now - s_blockStart;
    if (!s_stack.empty()) {
        s_stack.back().nested += idle;
    }
    if (s_session && (now > s_session->t0)) {
        s_session->idle.push_back(Interval { std::max(s_blockStart, s_session->t0), now });
    }
    s_blockStart = -1.;
}

void
onAboutToBlock()
{
    const double now = NodeCreationStageLog::nowMs();
    endIdle(now);
    s_blockStart = now;
}

void
onAwake()
{
    endIdle(NodeCreationStageLog::nowMs());
}

void
recordStage(const char* stage,
            double startMs,
            double endMs)
{
    Session& s = *s_session;
    if (s.stages.size() < kMaxStages) {
        s.stages.push_back(StageRecord { stage, startMs, endMs });
    }
    if ((std::strcmp(stage, "panelScheduled") == 0) || (std::strcmp(stage, "panelBuild") == 0)) {
        s.panelExpected = true;
    }
    s.lastActivityMs = std::max(s.lastActivityMs, endMs);
}

void
onStage(const char* stage,
        double startMs,
        double endMs)
{
    if (!s_sessionActive.load(std::memory_order_relaxed)) {
        return;
    }
    if (QThread::currentThread() != s_mainThread) {
        std::lock_guard<std::mutex> lock(s_offMainMutex);
        if (s_session) {
            std::pair<int, double>& entry = s_session->offMain[stage];
            ++entry.first;
            entry.second += endMs - startMs;
        }

        return;
    }
    if (!s_session || (endMs < s_session->t0)) {
        return;
    }
    recordStage(stage, startMs, endMs);
}

double
idleBetween(const Session& s,
            double from,
            double to)
{
    double idle = 0.;
    for (const Interval& i : s.idle) {
        const double a = std::max(i.start, from);
        const double b = std::min(i.end, to);
        if (b > a) {
            idle += b - a;
        }
    }

    return idle;
}

struct StageNode {
    const StageRecord* stage;
    std::vector<int> children;
};

class Report {
public:
    explicit Report(const Session& s)
        : _s(s)
    {
    }

    std::string build(const char* reason);

private:
    void line(const std::string& text)
    {
        _out += kPrefix;
        _out += text;
        _out += '\n';
    }

    double rel(double t) const
    {
        return t - _s.t0;
    }

    std::string busyIdleAt(double t) const
    {
        if (t <= _s.t0) {
            return format("%7.1f/%7.1f", 0., 0.);
        }
        const double idle = idleBetween(_s, _s.t0, t);

        return format("%7.1f/%7.1f", t - _s.t0 - idle, idle);
    }

    void buildForest();
    void printNode(int index, int depth);
    void printGroup(const std::vector<int>& members, int depth);
    void collectDescendants(int index, std::map<std::string, std::pair<int, double>>* into) const;
    void printGap(double from, double to);

    const Session& _s;
    std::vector<StageRecord> _sorted;
    std::vector<StageNode> _nodes;
    std::vector<int> _roots;
    std::size_t _timelineLines = 0;
    std::string _out;
};

void
Report::buildForest()
{
    _sorted = _s.stages;
    std::stable_sort(_sorted.begin(), _sorted.end(), [](const StageRecord& a, const StageRecord& b) {
        if (a.start != b.start) {
            return a.start < b.start;
        }

        return a.end > b.end;
    });
    _nodes.reserve(_sorted.size());
    std::vector<int> open;
    for (std::size_t i = 0; i < _sorted.size(); ++i) {
        const StageRecord& st = _sorted[i];
        while (!open.empty() && (st.start >= _nodes[open.back()].stage->end || st.end > _nodes[open.back()].stage->end)) {
            open.pop_back();
        }
        _nodes.push_back(StageNode { &st, std::vector<int>() });
        const int index = static_cast<int>(_nodes.size() - 1);
        if (open.empty()) {
            _roots.push_back(index);
        } else {
            _nodes[open.back()].children.push_back(index);
        }
        open.push_back(index);
    }
}

void
Report::collectDescendants(int index,
                           std::map<std::string, std::pair<int, double>>* into) const
{
    for (int child : _nodes[index].children) {
        const StageRecord& st = *_nodes[child].stage;
        std::pair<int, double>& entry = (*into)[st.name];
        ++entry.first;
        entry.second += st.end - st.start;
        collectDescendants(child, into);
    }
}

void
Report::printNode(int index,
                  int depth)
{
    if (_timelineLines++ >= kMaxTimelineLines) {
        return;
    }
    const StageRecord& st = *_nodes[index].stage;
    const std::string indent(2 * depth, ' ');
    if (st.end == st.start) {
        line(format("%8.1f        -  ", rel(st.start)) + busyIdleAt(st.start) + "  " + indent + st.name);
    } else {
        line(format("%8.1f %8.2f  ", rel(st.start), st.end - st.start) + busyIdleAt(st.start) + "  " + indent + st.name);
    }

    std::vector<std::string> order;
    std::map<std::string, std::vector<int>> groups;
    for (int child : _nodes[index].children) {
        const std::string& name = _nodes[child].stage->name;
        std::vector<int>& members = groups[name];
        if (members.empty()) {
            order.push_back(name);
        }
        members.push_back(child);
    }
    for (const std::string& name : order) {
        const std::vector<int>& members = groups[name];
        if (members.size() == 1) {
            printNode(members.front(), depth + 1);
        } else {
            printGroup(members, depth + 1);
        }
    }
}

void
Report::printGroup(const std::vector<int>& members,
                   int depth)
{
    if (_timelineLines++ >= kMaxTimelineLines) {
        return;
    }
    const StageRecord& first = *_nodes[members.front()].stage;
    double total = 0.;
    double lastEnd = first.end;
    std::map<std::string, std::pair<int, double>> descendants;
    for (int m : members) {
        const StageRecord& st = *_nodes[m].stage;
        total += st.end - st.start;
        lastEnd = std::max(lastEnd, st.end);
        collectDescendants(m, &descendants);
    }
    const std::string indent(2 * depth, ' ');
    line(format("%8.1f %8.2f  ", rel(first.start), total) + busyIdleAt(first.start) + "  " + indent + first.name + format(" x%.0f, last ends %+.1f", static_cast<double>(members.size()), rel(lastEnd)));

    std::vector<std::pair<std::string, std::pair<int, double>>> sorted(descendants.begin(), descendants.end());
    std::sort(sorted.begin(), sorted.end(), [](const std::pair<std::string, std::pair<int, double>>& a, const std::pair<std::string, std::pair<int, double>>& b) {
        return a.second.second > b.second.second;
    });
    const std::size_t shown = std::min<std::size_t>(sorted.size(), 8);
    for (std::size_t i = 0; i < shown; ++i) {
        if (_timelineLines++ >= kMaxTimelineLines) {
            return;
        }
        line(std::string(9, ' ') + format("%8.2f", sorted[i].second.second) + std::string(19, ' ') + indent + "  . " + sorted[i].first + format(" x%.0f, in total inside the stages above", static_cast<double>(sorted[i].second.first)));
    }
}

void
Report::printGap(double from,
                 double to)
{
    if (to - from <= kGapMs) {
        return;
    }
    const double idle = idleBetween(_s, from, to);
    const double busy = (to - from) - idle;
    std::map<std::string, double> byEvent;
    double attributed = 0.;
    for (const EventRecord& e : _s.events) {
        const double overlap = std::min(e.end, to) - std::max(e.start, from);
        if (overlap <= 0.) {
            continue;
        }
        const double share = std::min(e.self, overlap);
        byEvent[describe(e.type, e.className, e.ownerClass, e.objectName)] += share;
        attributed += share;
    }
    std::vector<std::pair<std::string, double>> sorted(byEvent.begin(), byEvent.end());
    std::sort(sorted.begin(), sorted.end(), [](const std::pair<std::string, double>& a, const std::pair<std::string, double>& b) {
        return a.second > b.second;
    });
    std::string text = format("%8.1f %8.2f  ", rel(from), to - from) + busyIdleAt(from) + format("  [gap: busy %.1f, idle %.1f]", busy, idle);
    std::size_t listed = 0;
    for (const std::pair<std::string, double>& entry : sorted) {
        if ((listed == 6) || (entry.second < 0.3)) {
            break;
        }
        text += format(" %.1f ", entry.second) + entry.first + ";";
        ++listed;
    }
    const double outside = busy - attributed;
    if (outside >= 0.5) {
        text += format(" %.1f outside any event", outside);
    }
    line(text);
    ++_timelineLines;
}

std::string
Report::build(const char* reason)
{
    buildForest();
    line("==== " + _s.label + " ====");
    line("t0 = start of " + _s.input + "; times in ms; busy/idle = main thread since t0");
    line("     +ms      dur     busy/   idle  stage");
    line(format("%8.1f        -  ", rel(_s.choiceMs)) + busyIdleAt(_s.choiceMs) + "  (choice handled)");

    double cursor = _s.t0;
    for (int root : _roots) {
        const StageRecord& st = *_nodes[root].stage;
        printGap(cursor, st.start);
        printNode(root, 0);
        cursor = std::max(cursor, st.end);
    }
    printGap(cursor, _s.lastActivityMs);
    if (_timelineLines >= kMaxTimelineLines) {
        line("(timeline cut at " + std::to_string(kMaxTimelineLines) + " lines)");
    }

    if (_s.panelShownMs >= 0.) {
        const double idle = idleBetween(_s, _s.t0, _s.panelShownMs);
        line(format("panel first painted %+.1f, its frame done %+.1f; until then the main thread was busy %.1f and idle %.1f",
                    rel(_s.panelPaintMs), rel(_s.panelShownMs), rel(_s.panelShownMs) - idle, idle));
    } else if (_s.panelExpected) {
        line("the panel never painted during the trace");
    } else {
        line("no properties panel was built");
    }
    {
        const double idle = idleBetween(_s, _s.t0, _s.lastActivityMs);
        line(format("last activity %+.1f: busy %.1f, idle %.1f; ", rel(_s.lastActivityMs), rel(_s.lastActivityMs) - idle, idle) + "trace ended: " + reason);
    }

    std::vector<const EventRecord*> slowest;
    for (const EventRecord& e : _s.events) {
        slowest.push_back(&e);
    }
    std::sort(slowest.begin(), slowest.end(), [](const EventRecord* a, const EventRecord* b) {
        return a->self > b->self;
    });
    line("slowest events (own time, excluding the events dispatched inside them and event-loop waits):");
    for (std::size_t i = 0; i < std::min<std::size_t>(slowest.size(), 12); ++i) {
        const EventRecord& e = *slowest[i];
        line(format("%8.1f %8.2f  own %.2f  ", rel(e.start), e.end - e.start, e.self) + describe(e.type, e.className, e.ownerClass, e.objectName));
    }
    line(format("%.0f events under 0.3 ms own time, %.1f ms in total", static_cast<double>(_s.smallEvents), _s.smallEventsMs));
    if (!_s.offMain.empty()) {
        std::string text = "stages on other threads:";
        for (const std::pair<const std::string, std::pair<int, double>>& entry : _s.offMain) {
            text += " " + entry.first + format(" x%.0f %.1f ms;", static_cast<double>(entry.second.first), entry.second.second);
        }
        line(text);
    }

    return _out;
}

void
flushReports()
{
    if (s_session || s_pendingReports.empty()) {
        return;
    }
    std::vector<QString> reports;
    reports.swap(s_pendingReports);
    AppInstancePtr app = appPTR ? appPTR->getTopLevelInstance() : AppInstancePtr();
    if (!app) {
        return;
    }
    for (const QString& r : reports) {
        app->appendToScriptEditor(r.toStdString());
    }
}

void
finish(const char* reason)
{
    if (!s_session) {
        return;
    }
    s_watchdog->stop();
    s_sessionActive.store(false, std::memory_order_relaxed);
    std::unique_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(s_offMainMutex);
        session.swap(s_session);
    }
    const std::string text = Report(*session).build(reason);
    std::fputs(text.c_str(), stderr);
    std::fflush(stderr);
    s_pendingReports.push_back(QString::fromStdString(text));
    QTimer::singleShot(0, qApp, []() { flushReports(); });
}

void
onWatchdog()
{
    if (!s_session) {
        s_watchdog->stop();

        return;
    }
    const Session& s = *s_session;
    const double now = NodeCreationStageLog::nowMs();
    const double elapsed = now - s.t0;
    const bool panelPending = s.panelExpected && (s.panelShownMs < 0.);
    if (elapsed >= kMaxSessionMs) {
        finish("time limit");
    } else if (!panelPending && (elapsed >= kMinSessionMs) && (now - s.lastActivityMs >= kQuietMs)) {
        finish("main thread quiet");
    }
}
} // anonymous namespace

bool
NodeCreationTrace::startFromEnvironment()
{
    if (qEnvironmentVariable("NATRON_TRACE_NODE_CREATION") != QLatin1String("1")) {
        return false;
    }
    if (s_enabled.load(std::memory_order_relaxed)) {
        return false;
    }
    QAbstractEventDispatcher* dispatcher = QAbstractEventDispatcher::instance();
    if (!dispatcher) {
        return false;
    }
    s_mainThread = QThread::currentThread();
    s_stack.reserve(64);
    QObject::connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, &onAboutToBlock);
    QObject::connect(dispatcher, &QAbstractEventDispatcher::awake, &onAwake);
    s_watchdog = new QTimer(qApp);
    s_watchdog->setInterval(kWatchdogMs);
    QObject::connect(s_watchdog, &QTimer::timeout, &onWatchdog);
    NodeCreationStageLog::setSink(&onStage);
    s_enabled.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "%stracing interactive node creations\n", kPrefix);

    return true;
}

void
NodeCreationTrace::begin(const char* origin,
                         const QString& pluginID)
{
    if (!enabled() || (QThread::currentThread() != s_mainThread)) {
        return;
    }
    if (s_session) {
        finish("the next creation started");
    }
    const double now = NodeCreationStageLog::nowMs();
    std::unique_ptr<Session> session(new Session);
    session->label = pluginID.toStdString() + " from " + origin;
    session->choiceMs = now;
    session->t0 = now;
    session->input = "the choice (no input event found)";
    for (int i = static_cast<int>(s_stack.size()) - 1; i >= 0; --i) {
        if (isInputEvent(s_stack[i].type)) {
            while ((i > 0) && isInputEvent(s_stack[i - 1].type)) {
                --i;
            }
            const Frame& f = s_stack[i];
            session->t0 = f.start;
            session->input = describe(f.type, f.className, f.ownerClass, f.objectName);
            break;
        }
    }
    session->lastActivityMs = now;
    {
        std::lock_guard<std::mutex> lock(s_offMainMutex);
        s_session.swap(session);
    }
    s_sessionActive.store(true, std::memory_order_relaxed);
    s_watchdog->start();
}

void
NodeCreationTrace::notePanelPainted()
{
    if (!s_session || (s_session->panelPaintMs >= 0.) || (QThread::currentThread() != s_mainThread)) {
        return;
    }
    Session& s = *s_session;
    s.panelPaintMs = NodeCreationStageLog::nowMs();
    s.panelExpected = true;
    recordStage("panelPainted", s.panelPaintMs, s.panelPaintMs);
    if (s_stack.empty()) {
        s.panelShownMs = s.panelPaintMs;

        return;
    }
    // The backing store is flushed to the window as the update request that painted returns.
    s.panelFrameDepth = 0;
    for (std::size_t i = 0; i < s_stack.size(); ++i) {
        const QEvent::Type type = s_stack[i].type;
        if ((type == QEvent::UpdateRequest) || (type == QEvent::UpdateLater) || (type == QEvent::Paint) || (type == QEvent::Expose)) {
            s.panelFrameDepth = static_cast<int>(i);
            break;
        }
    }
}

bool
NodeCreationTrace::push(QObject* receiver,
                        QEvent* e)
{
    if (QThread::currentThread() != s_mainThread) {
        return false;
    }
    const double now = NodeCreationStageLog::nowMs();
    endIdle(now);
    s_stack.emplace_back();
    Frame& f = s_stack.back();
    f.start = now;
    f.type = e->type();
    if (receiver) {
        f.className = receiver->metaObject()->className();
        f.objectName = receiver->objectName();
        if (QTimer* timer = qobject_cast<QTimer*>(receiver)) {
            f.own = (timer == s_watchdog);
            if (timer->parent()) {
                f.ownerClass = timer->parent()->metaObject()->className();
            }
        }
    }

    return true;
}

void
NodeCreationTrace::pop()
{
    if (s_stack.empty()) {
        return;
    }
    const double now = NodeCreationStageLog::nowMs();
    Frame f = std::move(s_stack.back());
    s_stack.pop_back();
    const double duration = now - f.start;
    const double self = std::max(0., duration - f.nested);
    if (!s_stack.empty()) {
        s_stack.back().nested += duration;
    }
    if (!s_session || (now < s_session->t0)) {
        return;
    }
    Session& s = *s_session;
    if (!f.own) {
        if (self >= kRecordedEventMs) {
            if (s.events.size() < kMaxEvents) {
                s.events.push_back(EventRecord { f.start, now, self, f.type, f.className, f.ownerClass, f.objectName });
            }
        } else {
            ++s.smallEvents;
            s.smallEventsMs += self;
        }
        if (self >= kActivityMs) {
            s.lastActivityMs = std::max(s.lastActivityMs, now);
        }
        if (f.type == QEvent::Expose) {
            recordStage(("expose " + describe(f.type, f.className, f.ownerClass, f.objectName)).c_str(), f.start, now);
        }
    }
    if ((s.panelFrameDepth >= 0) && (static_cast<int>(s_stack.size()) == s.panelFrameDepth)) {
        s.panelFrameDepth = -1;
        s.panelShownMs = now;
        recordStage("panelFrameShown", now, now);
    }
}

NATRON_NAMESPACE_EXIT
