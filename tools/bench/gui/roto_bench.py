"""Roto benchmark: pen-to-pixel latency of paint strokes and the cost of roto shape editing.

Runs inside the Natron GUI, normally through tools/bench/gui/run.sh --driver roto, which wraps
Tests/gui/run-gui-test.sh (Xvfb) or runs an AppImage on the host display:

    Tests/gui/run-gui-test.sh tools/bench/gui/roto_bench.py [natron-binary]

Ops, each with GUI_BENCH_REPS repetitions:
    stroke_<B>px   (topology roto-<format>, n = upstream nodes) a paced brush stroke on a RotoPaint
                   node, for every brush size B, project format and upstream graph size
    pen_up         (topology roto-<format>, n = upstream nodes) a short 50 px stroke; times the
                   release, which waits for the stroke's final render
    drag_point, drag_tangent, drag_feather, transform_box, hover
                   (topology roto-shapes, n = shapes) on one Roto node holding n shapes of
                   ROTO_BENCH_POINTS points keyed on ROTO_BENCH_KEYS frames; every drag is undone
                   through the Edit menu and checked

Configured by the environment:
    GUI_BENCH_REPS, GUI_BENCH_OPS, GUI_BENCH_OP_TIMEOUT_S, GUI_BENCH_SEED, GUI_BENCH_OUT
    (default $NATRON_GUI_TEST_OUT/roto_bench.jsonl), GUI_BENCH_MODE, GUI_BENCH_BUILD_TYPE,
    GUI_BENCH_GIT_SHA, GUI_BENCH_APPIMAGE, GUI_BENCH_WATCHDOG_S: as for gui_bench.py
    ROTO_BENCH_BRUSHES      brush sizes in image pixels (default 5,50,300)
    ROTO_BENCH_FORMATS      2k (2K_DCP) and/or 4k (UHD_4K) (default 2k,4k)
    ROTO_BENCH_UPSTREAM     upstream gen_graph comp sizes, 0 for none (default 0,300)
    ROTO_BENCH_SHAPES       shape counts (default 1,30,100)
    ROTO_BENCH_POINTS       control points per shape (default 24)
    ROTO_BENCH_KEYS         keyframes per shape (default 200)
    ROTO_BENCH_STROKE_EVENTS  pen moves per stroke (default 60)
    ROTO_BENCH_PEN_INTERVAL_MS  pause between pen moves, as a tablet's report rate (default 8)
    ROTO_BENCH_DRAG_EVENTS  mouse moves per drag or hover pass (default 20)
    NATRON_ROTO_LATENCY_LOG the latency probe's log; without it no latency is reported
    NATRON_STALL_LOG        the stall monitor's log; stalls are attributed as in gui_bench.py

Output: one host record, one setup record per stroke configuration and per shape count, then one
record per op in gui_bench.py's form. median_ms and p95_ms are the op's headline cost, which is
what compare.py diffs: for stroke_* the pen-to-pixel latency over every stroke point, for pen_up
the release, and for the shape ops the per-rep wall time (events forced through a synchronous
viewer repaint, so overlay cost lands in the event that caused it); the record's headline field
names it, and stroke ops fall back to the per-rep wall time without the probe. Every op record also carries
latency_p50_ms/latency_p95_ms, latency_points and latency_stages_ms (median per probe stage) when
the probe saw stroke points, overlay_p50_ms/overlay_p95_ms (timed synchronous viewer repaints)
and rep_median_ms (per-rep wall time).
"""
import json
import math
import os
import platform
import re
import socket
import statistics
import sys
import time
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))
sys.path.insert(0, os.path.join(ROOT, "tools", "bench", "gui"))

STARTUP_TIMEOUT_S = 120.0
SETTLE_MS = 300
RENDER_MS = 1500
STALL_FLUSH_MS = 120
LATENCY_QUIET_MS = 400
LATENCY_SETTLE_MAX_MS = 30000
FORMATS = {"2k": "2K_DCP", "4k": "UHD_4K"}
PEN_UP_BRUSH = 50
PEN_UP_EVENTS = 10
SHAPE_FORMAT = "2k"
STAGES = ("event_ms", "append_ms", "request_ms", "render_start_ms", "render_end_ms", "upload_ms", "paint_ms")


def env_list(name, default):
    return [v.strip() for v in os.environ.get(name, default).split(",") if v.strip()]


REPS = int(os.environ.get("GUI_BENCH_REPS", "5"))
ONLY_OPS = set(env_list("GUI_BENCH_OPS", ""))
OP_TIMEOUT_S = float(os.environ.get("GUI_BENCH_OP_TIMEOUT_S", "120"))
SEED = int(os.environ.get("GUI_BENCH_SEED", "1"))
WATCHDOG_S = int(os.environ.get("GUI_BENCH_WATCHDOG_S", str(6 * 3600)))
STALL_LOG = os.environ.get("NATRON_STALL_LOG", "")
LATENCY_LOG = os.environ.get("NATRON_ROTO_LATENCY_LOG", "")
BRUSHES = [int(v) for v in env_list("ROTO_BENCH_BRUSHES", "5,50,300")]
FORMAT_KEYS = env_list("ROTO_BENCH_FORMATS", "2k,4k")
UPSTREAM = [int(v) for v in env_list("ROTO_BENCH_UPSTREAM", "0,300")]
SHAPE_COUNTS = sorted(int(v) for v in env_list("ROTO_BENCH_SHAPES", "1,30,100"))
POINTS = int(os.environ.get("ROTO_BENCH_POINTS", "24"))
KEYS = int(os.environ.get("ROTO_BENCH_KEYS", "200"))
STROKE_EVENTS = int(os.environ.get("ROTO_BENCH_STROKE_EVENTS", "60"))
PEN_INTERVAL_MS = int(os.environ.get("ROTO_BENCH_PEN_INTERVAL_MS", "8"))
DRAG_EVENTS = int(os.environ.get("ROTO_BENCH_DRAG_EVENTS", "20"))

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import gen_graph
    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    OUT_PATH = os.environ.get("GUI_BENCH_OUT", os.path.join(gt.OUT, "roto_bench.jsonl"))
    _out_dir = os.path.dirname(os.path.abspath(OUT_PATH))
    if not os.path.isdir(_out_dir):
        os.makedirs(_out_dir)
    _out = open(OUT_PATH, "a")

    def emit(record):
        _out.write(json.dumps(record, sort_keys=True) + "\n")
        _out.flush()

    def mono_ms():
        # The C++ probes stamp std::chrono::steady_clock, which is this clock on Linux.
        return time.monotonic() * 1000.0

    # ---- host record ---------------------------------------------------------------------------

    def cpu_model():
        try:
            with open("/proc/cpuinfo") as f:
                for line in f:
                    if line.startswith("model name"):
                        return line.split(":", 1)[1].strip()
        except OSError:
            pass
        return platform.processor() or "unknown"

    def host_record():
        return {
            "type": "host",
            "driver": "roto",
            "hostname": socket.gethostname(),
            "cpu_model": cpu_model(),
            "cores": os.cpu_count(),
            "qt_platform": QGuiApplication.platformName(),
            "mode": os.environ.get("GUI_BENCH_MODE", "unknown"),
            "build_type": os.environ.get("GUI_BENCH_BUILD_TYPE", "unknown"),
            "git_sha": os.environ.get("GUI_BENCH_GIT_SHA", "unknown"),
            "appimage": bool(os.environ.get("GUI_BENCH_APPIMAGE")),
            "natron_version": NatronEngine.natron.getNatronVersionString(),
            "stall_log": bool(STALL_LOG),
            "latency_log": bool(LATENCY_LOG),
            "reps": REPS,
            "seed": SEED,
            "op_timeout_s": OP_TIMEOUT_S,
            "pen_interval_ms": PEN_INTERVAL_MS,
            "stroke_events": STROKE_EVENTS,
            "drag_events": DRAG_EVENTS,
            "points": POINTS,
            "keys": KEYS,
            "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }

    # ---- logs ----------------------------------------------------------------------------------

    def read_jsonl(path):
        if not path or not os.path.isfile(path):
            return []
        out = []
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except ValueError:
                    continue
        return out

    def attribute_stalls(windows):
        """Stalls whose midpoint falls in one of windows."""
        mine = []
        for s in read_jsonl(STALL_LOG):
            try:
                start, duration = float(s["start_ms"]), float(s["duration_ms"])
            except (KeyError, TypeError, ValueError):
                continue
            if any(t0 <= start + duration / 2.0 <= t1 for t0, t1 in windows):
                mine.append(duration)
        return {"count": len(mine), "max_ms": round(max(mine), 3) if mine else 0.0,
                "total_ms": round(sum(mine), 3)}

    def p95(values):
        ordered = sorted(values)
        return ordered[max(0, int(math.ceil(0.95 * len(ordered))) - 1)]

    def latency_stats(windows):
        """Pen-to-pixel figures for the stroke points whose input event falls in one of windows.
        A point is logged once its pixels are painted, so a point never painted is not counted."""
        points = []
        for r in read_jsonl(LATENCY_LOG):
            ev = r.get("event_ms")
            if ev is not None and any(t0 <= ev <= t1 for t0, t1 in windows):
                points.append(r)
        out = {"latency_points": len(points), "latency_p50_ms": None, "latency_p95_ms": None}
        totals = [r["paint_ms"] - r["event_ms"] for r in points if r.get("paint_ms") is not None]
        if totals:
            out["latency_p50_ms"] = round(statistics.median(totals), 3)
            out["latency_p95_ms"] = round(p95(totals), 3)
        stages = {}
        for a, b in zip(STAGES, STAGES[1:]):
            d = [r[b] - r[a] for r in points if r.get(a) is not None and r.get(b) is not None]
            stages[a[:-3] + "_to_" + b[:-3]] = round(statistics.median(d), 3) if d else None
        out["latency_stages_ms"] = stages
        out["_totals"] = totals
        return out

    def latency_settle():
        """Waits until the latency log has stopped growing: the stroke's renders have landed."""
        if not LATENCY_LOG:
            yield ("sleep", RENDER_MS)
            return
        start = mono_ms()
        last_size, last_change = -1, start
        while mono_ms() - start < LATENCY_SETTLE_MAX_MS:
            size = os.path.getsize(LATENCY_LOG) if os.path.isfile(LATENCY_LOG) else 0
            if size != last_size:
                last_size, last_change = size, mono_ms()
            elif mono_ms() - last_change >= LATENCY_QUIET_MS:
                return
            yield ("sleep", 50)

    # ---- GUI plumbing --------------------------------------------------------------------------

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def activate():
        win = main_window()
        if win is None:
            return
        win.activateWindow()
        win.raise_()
        deadline = time.time() + 10.0
        while not win.isActiveWindow() and time.time() < deadline:
            QApplication.processEvents()
            time.sleep(0.02)

    def edit_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().replace("&", "").lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().replace("&", "").lower().startswith(prefix):
                        return a
        return None

    def trigger(prefix):
        a = edit_action(prefix)
        if a is None or not a.isEnabled():
            raise RuntimeError("no enabled %s action in the Edit menu" % prefix)
        a.trigger()

    def viewer_gl():
        visible = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return visible[0] if visible else None

    def info_bar():
        bars = [w for w in gt.widgets_of("InfoViewerWidget") if w.isVisible()]
        return bars[0] if bars else None

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    def strip_tags(text):
        return re.sub(r"<[^>]*>", "", text).replace("&nbsp;", " ").strip()

    def info_mouse():
        bar = info_bar()
        if bar is None:
            return None
        for w in QApplication.allWidgets():
            try:
                if gt.class_name(w) == "Label" and ancestor_of_class(w, "InfoViewerWidget") is bar:
                    m = re.match(r"x=(-?\d+) y=(-?\d+)", strip_tags(w.text()))
                    if m:
                        return (int(m.group(1)), int(m.group(2)))
            except RuntimeError:
                continue
        return None

    def send_mouse(widget, etype, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, modifiers))
        QApplication.processEvents()

    def press(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonPress, pos, button, button, modifiers)

    def release(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonRelease, pos, button, Qt.NoButton, modifiers)

    def move(widget, pos, buttons=Qt.NoButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, buttons, modifiers)

    def click(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        press(widget, pos, button, modifiers)
        release(widget, pos, button, modifiers)

    def send_key(widget, key, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    def select_all_points(gl):
        # With one shape selected, Ctrl+A selects all of its points; the interact tracks Control
        # from its own key events.
        QApplication.sendEvent(gl, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
        send_key(gl, Qt.Key_A, Qt.ControlModifier)
        QApplication.sendEvent(gl, QKeyEvent(QEvent.KeyRelease, Qt.Key_Control, Qt.NoModifier))
        QApplication.processEvents()

    def timed_repaint(gl, into):
        t0 = mono_ms()
        gl.repaint()
        into.append(mono_ms() - t0)

    class ViewerMap(object):
        """Widget pixels to canonical coordinates, calibrated from the info bar's mouse readout:
        it shows whole pixels only, so each axis is found from two positions where the readout
        steps to the next pixel, by bisection between hover moves."""

        def __init__(self, gl):
            self.gl = gl
            w, h = float(gl.width()), float(gl.height())
            self.ok = True
            (x1, cx1), (x2, cx2) = self._edge(0, w * 0.1, w * 0.5, h / 2.0), self._edge(0, w * 0.6, w * 0.9, h / 2.0)
            (y1, cy1), (y2, cy2) = self._edge(1, h * 0.1, h * 0.5, w / 2.0), self._edge(1, h * 0.6, h * 0.9, w / 2.0)
            if None in (x1, x2, y1, y2) or x1 == x2 or y1 == y2:
                self.ok = False
                return
            self.sx = (cx2 - cx1) / (x2 - x1)
            self.sy = (cy2 - cy1) / (y2 - y1)
            self.p0 = QPointF(x1, y1)
            self.c0 = (cx1, cy1)

        def _coord(self, axis, pos, other):
            p = QPointF(pos, other) if axis == 0 else QPointF(other, pos)
            move(self.gl, p)
            c = info_mouse()
            return None if c is None else c[axis]

        def _edge(self, axis, lo, hi, other):
            c_lo = self._coord(axis, lo, other)
            c_hi = self._coord(axis, hi, other)
            if c_lo is None or c_hi is None or c_lo == c_hi:
                return (None, None)
            for _ in range(30):
                if hi - lo < 0.05:
                    break
                mid = (lo + hi) / 2.0
                if self._coord(axis, mid, other) == c_lo:
                    lo = mid
                else:
                    hi = mid
            stepped = self._coord(axis, hi, other)
            # Going down the readout steps to c_lo - 1 (y is up): the boundary is the larger value.
            return (hi, max(c_lo, stepped))

        def to_canonical(self, p):
            return (self.c0[0] + (p.x() - self.p0.x()) * self.sx, self.c0[1] + (p.y() - self.p0.y()) * self.sy)

        def to_widget(self, x, y):
            return QPointF(self.p0.x() + (x - self.c0[0]) / self.sx, self.p0.y() + (y - self.c0[1]) / self.sy)

        def pixel_scale(self):
            return abs(self.sx)

    # ---- project ---------------------------------------------------------------------------------

    _state = {}

    def viewer_node():
        return _state["viewer"]

    def create(plugin_id, x, y):
        # An open settings panel makes the node's viewer interact the active one.
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def view(node):
        v = viewer_node()
        if v.getInput(0) is not None:
            v.disconnectInput(0)
        if node is not None:
            gt.require(v.connectInput(0, node), "the Viewer shows %s" % node.getLabel())
        QApplication.processEvents()

    def set_format(key):
        name = FORMATS[key]
        fmt = app.getProjectParam("outputFormat")
        options = [str(o) for o in fmt.getOptions()]
        index = next((i for i, o in enumerate(options) if name in o), None)
        gt.require(index is not None, "the project offers the %s format" % name)
        fmt.setValue(index)
        QApplication.processEvents()

    def fit_viewer(gl):
        move(gl, QPointF(gl.width() / 2.0, gl.height() / 2.0))
        send_key(gl, Qt.Key_F)

    def comp_tail(summary):
        """The generated node with the most upstream nodes that feeds no other node."""
        names = summary["root_nodes"]
        nodes = dict((nm, app.getNode(nm)) for nm in names)
        inputs, consumed = {}, set()
        for nm, node in nodes.items():
            ins = []
            for i in range(node.getMaxInputCount()):
                inp = node.getInput(i)
                if inp is not None:
                    ins.append(inp.getScriptName())
            inputs[nm] = ins
            consumed.update(ins)
        best, best_count = None, -1
        for nm in names:
            if nm in consumed:
                continue
            seen, stack = set(), list(inputs[nm])
            while stack:
                cur = stack.pop()
                if cur not in seen:
                    seen.add(cur)
                    stack.extend(inputs.get(cur, ()))
            if len(seen) > best_count:
                best, best_count = nm, len(seen)
        return nodes.get(best)

    def clear_strokes(roto_ctx):
        layer = roto_ctx.getBaseLayer()
        for item in list(layer.getChildren()):
            layer.removeItem(item)

    # ---- driver --------------------------------------------------------------------------------

    def selected(name):
        return not ONLY_OPS or name in ONLY_OPS or name.split("_")[0] in ONLY_OPS

    def short_error():
        return traceback.format_exc().strip().splitlines()[-1]

    def steps_of(result):
        """Commands of a hook that is a generator; nothing for a plain function."""
        if result is not None and hasattr(result, "send"):
            for cmd in result:
                yield cmd

    def run_op(name, op, topology, n, extra):
        """Like gui_bench.run_op; op["after"] may also be a generator, run outside the timed
        window, and op["headline"] picks the values behind median_ms (default the rep times)."""
        reps = op.get("reps", REPS)
        st = {"overlay": [], "headline": []}
        times, windows = [], []
        event_max = 0.0
        events_per_rep = 0
        status, error = "ok", None
        deadline = time.monotonic() + OP_TIMEOUT_S
        try:
            if "setup" in op:
                for cmd in steps_of(op["setup"](st)):
                    yield cmd
            yield ("sleep", 0)
            for rep in range(reps):
                if "before" in op:
                    for cmd in steps_of(op["before"](st, rep)):
                        yield cmd
                yield ("sleep", 0)
                gen = op["events"](st, rep)
                count = 0
                t0 = mono_ms()
                while True:
                    e0 = mono_ms()
                    try:
                        cmd = next(gen)
                        done = False
                    except StopIteration:
                        done = True
                    event_max = max(event_max, mono_ms() - e0)
                    if done:
                        break
                    count += 1
                    if time.monotonic() > deadline:
                        status = "timeout"
                        gen.close()
                        break
                    yield cmd if cmd else ("sleep", 0)
                t1 = mono_ms()
                times.append(t1 - t0)
                windows.append((t0, t1))
                events_per_rep = count
                if "after" in op:
                    for cmd in steps_of(op["after"](st, rep)):
                        yield cmd
                if status != "ok":
                    break
        except Exception:
            status, error = "error", short_error()
        try:
            if "teardown" in op:
                for cmd in steps_of(op["teardown"](st)):
                    yield cmd
        except Exception:
            if status == "ok":
                status, error = "error", "teardown: " + short_error()
        yield ("sleep", STALL_FLUSH_MS)
        record = {
            "type": "op", "op": name, "kind": "roto", "topology": topology, "n": n,
            "status": status, "reps": len(times), "events_per_rep": events_per_rep,
            "per_event_max_ms": round(event_max, 3),
            "stalls": attribute_stalls(windows),
        }
        lat = latency_stats(windows)
        totals = lat.pop("_totals")
        record.update(lat)
        if times:
            record["rep_median_ms"] = round(statistics.median(times), 3)
        metric = op.get("headline", "rep")
        if metric == "latency" and not LATENCY_LOG:
            metric = "rep"
        record["headline"] = metric
        headline = {"rep": times, "latency": totals, "values": st["headline"]}[metric]
        if headline:
            record.update({"median_ms": round(statistics.median(headline), 3), "p95_ms": round(p95(headline), 3),
                           "min_ms": round(min(headline), 3), "max_ms": round(max(headline), 3)})
        elif status == "ok":
            status, error = "error", "no %s values were measured" % metric
            record["status"] = status
        if st["overlay"]:
            record["overlay_p50_ms"] = round(statistics.median(st["overlay"]), 3)
            record["overlay_p95_ms"] = round(p95(st["overlay"]), 3)
        if error:
            record["error"] = error
        record.update(extra)
        record.update(st.get("record", {}))
        emit(record)
        report("RESULT " + json.dumps(record, sort_keys=True))
        check(status == "ok", "%s %s n=%d: %s%s" % (name, topology, n, status, (" (" + error + ")") if error else ""))

    # ---- strokes ---------------------------------------------------------------------------------

    class StrokeCtx(object):
        def __init__(self, node, gl, vmap):
            self.node = node
            self.roto = node.getRotoContext()
            self.gl = gl
            self.vmap = vmap

        def path(self, events):
            gl = self.gl
            cx, cy = gl.width() / 2.0, gl.height() / 2.0
            r = 0.3 * min(gl.width(), gl.height())
            return [QPointF(cx + r * math.cos(2 * math.pi * i / events), cy + 0.8 * r * math.sin(4 * math.pi * i / events))
                    for i in range(events + 1)]

    def stroke_op(sc, brush, events, pen_up):
        def setup(st):
            sc.node.getParam("sizeSpinbox").setValue(float(brush))
            yield ("sleep", SETTLE_MS)

        def events_gen(st, rep):
            pts = sc.path(events)
            press(sc.gl, pts[0])
            yield ("sleep", PEN_INTERVAL_MS)
            for p in pts[1:]:
                move(sc.gl, p, Qt.LeftButton)
                yield ("sleep", PEN_INTERVAL_MS)
            # The release waits for the stroke's final render on the main thread.
            t0 = mono_ms()
            QApplication.sendEvent(sc.gl, QMouseEvent(QEvent.MouseButtonRelease, QPointF(pts[-1]), Qt.LeftButton,
                                                      Qt.NoButton, Qt.NoModifier))
            st["release"] = mono_ms() - t0
            QApplication.processEvents()
            yield ("sleep", 0)

        def after(st, rep):
            if pen_up:
                st["headline"].append(st["release"])
            for cmd in latency_settle():
                yield cmd
            # The stroke stays drawn under an idle cursor: the frame cost of the overlay over it.
            move(sc.gl, QPointF(sc.gl.width() / 2.0, sc.gl.height() / 2.0))
            for _ in range(3):
                timed_repaint(sc.gl, st["overlay"])
            clear_strokes(sc.roto)
            for cmd in latency_settle():
                yield cmd

        return {"setup": setup, "events": events_gen, "after": after,
                "headline": "values" if pen_up else "latency"}

    def stroke_phase():
        comp_tail_node = None
        for upstream in UPSTREAM:
            if upstream > 0 and comp_tail_node is None:
                # Built before the RotoPaint, so closing the generated nodes' panels leaves the
                # RotoPaint's panel, and with it its viewer interact, as the only one open.
                view(None)
                t0 = mono_ms()
                summary = gen_graph.build(app, "comp", upstream, SEED)
                comp_tail_node = comp_tail(summary)
                gt.require(comp_tail_node is not None, "found the end of the %d-node comp" % upstream)
                emit({"type": "setup", "what": "upstream", "topology": "comp", "n": upstream,
                      "nodes": summary["nodes"], "gen_ms": round(mono_ms() - t0, 3)})
            gt.close_all_panels()
            rp = create("fr.inria.built-in.RotoPaint", 0, 0)
            gt.require(rp.getRotoContext() is not None, "the RotoPaint node has a roto context")
            if upstream > 0:
                gt.require(rp.connectInput(0, comp_tail_node), "the RotoPaint's Bg is the end of the comp")
            for fkey in FORMAT_KEYS:
                gt.section("stroke-%s-%d" % (fkey, upstream))
                set_format(fkey)
                view(rp)
                yield ("sleep", RENDER_MS)
                gl = viewer_gl()
                gt.require(gl is not None, "the viewer is visible")
                fit_viewer(gl)
                yield ("sleep", RENDER_MS)
                vmap = ViewerMap(gl)
                gt.require(vmap.ok, "calibrated the viewer at %s" % fkey)
                sc = StrokeCtx(rp, gl, vmap)
                topology = "roto-" + fkey
                extra = {"format": FORMATS[fkey], "upstream": upstream, "pixel_scale": round(vmap.pixel_scale(), 4)}
                for brush in BRUSHES:
                    name = "stroke_%dpx" % brush
                    if selected(name):
                        e = dict(extra, brush_px=brush)
                        for cmd in run_op(name, stroke_op(sc, brush, STROKE_EVENTS, False), topology, upstream, e):
                            yield cmd
                if selected("pen_up"):
                    e = dict(extra, brush_px=PEN_UP_BRUSH)
                    for cmd in run_op("pen_up", stroke_op(sc, PEN_UP_BRUSH, PEN_UP_EVENTS, True), topology, upstream, e):
                        yield cmd
            view(None)
            rp.destroy(False)
            yield ("sleep", SETTLE_MS)

    # ---- shapes ----------------------------------------------------------------------------------

    def shape_points(cx, cy, r, tangent):
        """Clockwise from the top: (x, y, lx, ly, rx, ry) per point."""
        pts = []
        for i in range(POINTS):
            a = math.pi / 2 - 2 * math.pi * i / POINTS
            x, y = cx + r * math.cos(a), cy + r * math.sin(a)
            tx, ty = math.sin(a), -math.cos(a)
            pts.append((x, y, x - tx * tangent, y - ty * tangent, x + tx * tangent, y + ty * tangent))
        return pts

    def add_shape(roto, pts, t_first, wobble):
        bez = roto.createBezier(pts[0][0], pts[0][1], t_first)
        if bez is None:
            raise RuntimeError("createBezier failed")
        for p in pts[1:]:
            bez.addControlPoint(p[0], p[1])
        bez.setCurveFinished(True)
        # Time-major, so the first point set at a frame keys the whole shape there once.
        for k in range(KEYS):
            t = t_first + k
            d = wobble * math.sin(0.1 * k)
            for i, p in enumerate(pts):
                q = (p[0] + d, p[1], p[2] + d, p[3], p[4] + d, p[5])
                bez.setPointAtIndex(i, t, *q)
                bez.setFeatherPointAtIndex(i, t, *q)
        return bez

    class ShapeCtx(object):
        def __init__(self, node, gl, vmap, t):
            self.node = node
            self.roto = node.getRotoContext()
            self.gl = gl
            self.vmap = vmap
            self.t = t
            w, h = float(gl.width()), float(gl.height())
            self.centre = QPointF(w / 2.0, h / 2.0)
            # Point spacing must stay well above the 8 px hit tolerances, and tangent ends clear
            # of both their own point and the neighbours' tangents.
            self.radius_px = 0.22 * min(w, h)
            self.tangent_px = 0.33 * (2 * math.pi * self.radius_px / POINTS)
            ps = vmap.pixel_scale()
            cx, cy = vmap.to_canonical(self.centre)
            self.target_pts = shape_points(cx, cy, self.radius_px * ps, self.tangent_px * ps)
            self.target = None
            self.shapes = []
            self.cells = self._cells(w, h)

        def _cells(self, w, h):
            """Grid cell centres and radius for the other shapes, clear of the target."""
            cols, rows = 16, 12
            cw, ch = w / cols, h / rows
            r_px = 0.35 * min(cw, ch)
            keep_out = self.radius_px + self.tangent_px + 30.0
            cells = []
            for j in range(rows):
                for i in range(cols):
                    c = QPointF((i + 0.5) * cw, (j + 0.5) * ch)
                    if abs(c.x() - self.centre.x()) < keep_out + r_px and abs(c.y() - self.centre.y()) < keep_out + r_px:
                        continue
                    cells.append(c)
            return cells, r_px

        def grow(self, count):
            ps = self.vmap.pixel_scale()
            if self.target is None:
                self.target = add_shape(self.roto, self.target_pts, self.t, 2.0 * ps)
                self.shapes.append(self.target)
                yield ("sleep", 0)
            cells, r_px = self.cells
            while len(self.shapes) < count:
                k = len(self.shapes) - 1
                if k >= len(cells):
                    raise RuntimeError("the viewer has room for %d shapes only" % (len(cells) + 1))
                cx, cy = self.vmap.to_canonical(cells[k])
                pts = shape_points(cx, cy, r_px * ps, 0.25 * r_px * ps)
                self.shapes.append(add_shape(self.roto, pts, self.t, 2.0 * ps))
                yield ("sleep", 0)

        def snapshot(self):
            out = []
            for i in range(POINTS):
                out.append(tuple(self.target.getControlPointPosition(i, self.t)))
                out.append(tuple(self.target.getFeatherPointPosition(i, self.t)))
            return out

        def widget_of(self, x, y):
            return self.vmap.to_widget(x, y)

        def empty_spot(self):
            return QPointF(self.centre.x() + self.radius_px + 25.0, self.centre.y())

    def same_snapshot(a, b):
        return all(abs(u - v) < 1e-6 for pa, pb in zip(a, b) for u, v in zip(pa, pb))

    def drag_op(sc, kind):
        """A drag of the target shape's top point, its right tangent, its feather bar or the
        transform box of all its points; each rep is undone and checked."""
        dist = 40.0

        def before(st, rep):
            gl = sc.gl
            st["original"] = sc.snapshot()
            click(gl, sc.empty_spot())
            top = sc.target_pts[0]
            if kind in ("drag_tangent", "drag_feather", "transform_box"):
                click(gl, sc.widget_of(top[0], top[1]))
            if kind == "transform_box":
                select_all_points(gl)
            yield ("sleep", SETTLE_MS)
            ps = sc.vmap.pixel_scale()
            if kind == "drag_point":
                start, delta = sc.widget_of(top[0], top[1]), QPointF(dist, 0)
            elif kind == "drag_tangent":
                start, delta = sc.widget_of(top[4], top[5]), QPointF(0, dist)
            elif kind == "drag_feather":
                # The feather bar sticks out of the point along its outward normal, up for the top point.
                start, delta = sc.widget_of(top[0], top[1] + 14 * ps), QPointF(0, -dist)
            else:
                xs = [v for p in sc.target_pts for v in (p[0], p[2], p[4])]
                ys = [v for p in sc.target_pts for v in (p[1], p[3], p[5])]
                # Top-left corner, on its scale arrow, dragged outward.
                start = sc.widget_of(min(xs) - 7.5 * ps, max(ys) + 7.5 * ps)
                delta = QPointF(-dist, -dist)
            st["start"], st["delta"] = start, delta

        def events(st, rep):
            gl = sc.gl
            start, delta = st["start"], st["delta"]
            press(gl, start)
            timed_repaint(gl, st["overlay"])
            yield None
            for i in range(1, DRAG_EVENTS + 1):
                move(gl, start + delta * (i / float(DRAG_EVENTS)), Qt.LeftButton)
                timed_repaint(gl, st["overlay"])
                yield None
            release(gl, start + delta)
            timed_repaint(gl, st["overlay"])
            yield None

        def after(st, rep):
            yield ("sleep", SETTLE_MS)
            if same_snapshot(sc.snapshot(), st["original"]):
                raise RuntimeError("the %s drag did not move the shape" % kind)
            for _ in range(DRAG_EVENTS + 3):
                if same_snapshot(sc.snapshot(), st["original"]):
                    break
                trigger("undo")
                QApplication.processEvents()
            if not same_snapshot(sc.snapshot(), st["original"]):
                raise RuntimeError("undo did not restore the shape after %s" % kind)
            yield ("sleep", SETTLE_MS)

        return {"before": before, "events": events, "after": after}

    def hover_op(sc):
        """The cursor crosses the viewer over every shape with no button down."""
        def before(st, rep):
            click(sc.gl, sc.empty_spot())
            yield ("sleep", SETTLE_MS)

        def events(st, rep):
            gl = sc.gl
            w, h = float(gl.width()), float(gl.height())
            n = max(1, DRAG_EVENTS)
            for i in range(n + 1):
                f = i / float(n)
                move(gl, QPointF(0.05 * w + 0.9 * w * f, 0.5 * h + 0.4 * h * math.sin(2 * math.pi * f)))
                timed_repaint(gl, st["overlay"])
                yield None

        return {"before": before, "events": events}

    def shape_phase():
        set_format(SHAPE_FORMAT)
        gt.close_all_panels()
        node = create("fr.inria.built-in.Roto", 0, 400)
        gt.require(node.getRotoContext() is not None, "the Roto node has a roto context")
        view(node)
        yield ("sleep", RENDER_MS)
        gl = viewer_gl()
        gt.require(gl is not None, "the viewer is visible")
        fit_viewer(gl)
        yield ("sleep", RENDER_MS)
        vmap = ViewerMap(gl)
        gt.require(vmap.ok, "calibrated the viewer for the shapes")
        # The Roto viewer starts on the Bezier drawing tool; Q switches to selection.
        send_key(gl, Qt.Key_Q)
        sc = ShapeCtx(node, gl, vmap, app.timelineGetTime())
        for count in SHAPE_COUNTS:
            gt.section("shapes-%d" % count)
            view(None)
            t0 = mono_ms()
            for cmd in sc.grow(count):
                yield cmd
            emit({"type": "setup", "what": "shapes", "n": count, "points": POINTS, "keys": KEYS,
                  "build_ms": round(mono_ms() - t0, 3)})
            view(node)
            yield ("sleep", RENDER_MS)
            extra = {"format": FORMATS[SHAPE_FORMAT], "shapes": count, "points": POINTS, "keys": KEYS}
            for kind in ("drag_point", "drag_tangent", "drag_feather", "transform_box"):
                if selected(kind):
                    for cmd in run_op(kind, drag_op(sc, kind), "roto-shapes", count, extra):
                        yield cmd
            if selected("hover"):
                for cmd in run_op("hover", hover_op(sc), "roto-shapes", count, extra):
                    yield cmd
        view(None)
        node.destroy(False)
        yield ("sleep", SETTLE_MS)

    def steps():
        ok = yield ("until", lambda: main_window() is not None and viewer_gl() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the main window and viewer are up")
        gt.close_all_panels()
        activate()
        if not STALL_LOG:
            report("NOTE NATRON_STALL_LOG is not set; no stalls will be attributed")
        if not LATENCY_LOG:
            report("NOTE NATRON_ROTO_LATENCY_LOG is not set; no pen-to-pixel latency will be reported")
        emit(host_record())
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("fr.inria.built-in.Viewer")
        gt.require(viewer is not None, "a Viewer node exists")
        _state["viewer"] = viewer
        if any(selected("stroke_%dpx" % b) for b in BRUSHES) or selected("pen_up"):
            for cmd in stroke_phase():
                yield cmd
            if not app.resetProject():
                raise RuntimeError("resetProject failed")
            yield ("sleep", SETTLE_MS)
            viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
            gt.require(viewer is not None, "a Viewer node exists after the reset")
            _state["viewer"] = viewer
        if any(selected(k) for k in ("drag_point", "drag_tangent", "drag_feather", "transform_box", "hover")):
            for cmd in shape_phase():
                yield cmd
        _out.close()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
