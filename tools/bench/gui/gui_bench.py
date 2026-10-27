"""GUI benchmark: times node graph interaction and edit operations on generated large graphs.

Runs inside the Natron GUI, normally through tools/bench/gui/run.sh, which wraps
Tests/gui/run-gui-test.sh (Xvfb) or runs an AppImage on the host display:

    Tests/gui/run-gui-test.sh tools/bench/gui/gui_bench.py [natron-binary]

Configured by the environment:
    GUI_BENCH_TOPOLOGIES  comma-separated gen_graph topologies (default mixed)
    GUI_BENCH_SIZES       comma-separated node counts (default 300)
    GUI_BENCH_REPS        repetitions per op (default 5)
    GUI_BENCH_OPS         comma-separated op names to run (default all)
    GUI_BENCH_OP_TIMEOUT_S  per-op budget; an op past it is recorded as "timeout" (default 120)
    GUI_BENCH_SEED        generator seed (default 1)
    GUI_BENCH_IDLE_S      length of the idle op (default 5)
    GUI_BENCH_SELF_CHECK  1 adds an op that blocks the main thread for 300 ms once and checks
                          that the stall log attributes exactly one stall to it
    GUI_BENCH_OUT         JSON-lines output (default $NATRON_GUI_TEST_OUT/gui_bench.jsonl)
    GUI_BENCH_MODE, GUI_BENCH_BUILD_TYPE, GUI_BENCH_GIT_SHA, GUI_BENCH_APPIMAGE
                          copied into the host record
    NATRON_STALL_LOG      the stall monitor's log; stalls are attributed to the op whose timed
                          window holds their midpoint

Output: one host record, then per (topology, n) one graph record and one record per op with the
median, p95, min and max of the per-rep wall times, the longest single event, and the stalls.
Each rep's events run on the Qt event loop, one at a time, so timers and deferred paints land
between them as they would for a user; interaction events force a synchronous viewport repaint,
so paint cost lands inside the event that caused it.
"""
import json
import math
import os
import platform
import re
import socket
import statistics
import sys
import tempfile
import time
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))
sys.path.insert(0, os.path.join(ROOT, "tools", "bench", "gui"))

STARTUP_TIMEOUT_S = 120.0
SETTLE_MS = 300
STALL_FLUSH_MS = 120
DRAG_EVENTS = 40


def env_list(name, default):
    return [v.strip() for v in os.environ.get(name, default).split(",") if v.strip()]


TOPOLOGIES = env_list("GUI_BENCH_TOPOLOGIES", "mixed")
SIZES = [int(v) for v in env_list("GUI_BENCH_SIZES", "300")]
REPS = int(os.environ.get("GUI_BENCH_REPS", "5"))
ONLY_OPS = set(env_list("GUI_BENCH_OPS", ""))
OP_TIMEOUT_S = float(os.environ.get("GUI_BENCH_OP_TIMEOUT_S", "120"))
SEED = int(os.environ.get("GUI_BENCH_SEED", "1"))
IDLE_S = float(os.environ.get("GUI_BENCH_IDLE_S", "5"))
SELF_CHECK = os.environ.get("GUI_BENCH_SELF_CHECK", "0") == "1"
STALL_LOG = os.environ.get("NATRON_STALL_LOG", "")
WATCHDOG_S = int(os.environ.get("GUI_BENCH_WATCHDOG_S", str(6 * 3600)))

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent, QWheelEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    import gen_graph
    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    OUT_PATH = os.environ.get("GUI_BENCH_OUT", os.path.join(gt.OUT, "gui_bench.jsonl"))
    _out_dir = os.path.dirname(os.path.abspath(OUT_PATH))
    if not os.path.isdir(_out_dir):
        os.makedirs(_out_dir)
    _out = open(OUT_PATH, "a")

    def emit(record):
        _out.write(json.dumps(record, sort_keys=True) + "\n")
        _out.flush()

    def mono_ms():
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
            "reps": REPS,
            "seed": SEED,
            "op_timeout_s": OP_TIMEOUT_S,
            "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }

    # ---- stalls --------------------------------------------------------------------------------

    def read_stalls():
        if not STALL_LOG or not os.path.isfile(STALL_LOG):
            return []
        stalls = []
        with open(STALL_LOG) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    s = json.loads(line)
                    stalls.append((float(s["start_ms"]), float(s["duration_ms"])))
                except (ValueError, KeyError):
                    continue
        return stalls

    def attribute_stalls(windows):
        """Stalls whose midpoint falls in one of windows. A stall starts at the last heartbeat
        before the block, up to a heartbeat interval before the event that caused it."""
        mine = [d for s, d in read_stalls() if any(t0 <= s + d / 2.0 <= t1 for t0, t1 in windows)]
        return {"count": len(mine), "max_ms": round(max(mine), 3) if mine else 0.0,
                "total_ms": round(sum(mine), 3)}

    # ---- GUI plumbing --------------------------------------------------------------------------

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

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

    def invoke(slot):
        QMetaObject.invokeMethod(root_graph(), slot)

    def repaint():
        root_graph().viewport().repaint()

    def fit():
        invoke("centerOnAllNodes")
        repaint()

    def to_view(x, y):
        p = root_graph().mapFromScene(QPointF(x, y))
        return QPointF(p.x(), p.y())

    def node_centre(name):
        node = app.getNode(name)
        x, y = node.getPosition()
        w, h = node.getSize()
        return x + w / 2.0, y + h / 2.0

    def mouse(etype, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(root_graph().viewport(), QMouseEvent(etype, pos, button, buttons, modifiers))

    def wheel(pos, steps):
        vp = root_graph().viewport()
        glob = QPointF(vp.mapToGlobal(QPoint(int(pos.x()), int(pos.y()))))
        QApplication.sendEvent(vp, QWheelEvent(pos, glob, QPoint(0, 0), QPoint(0, 120 * steps), Qt.NoButton,
                                               Qt.NoModifier, Qt.NoScrollPhase, False))

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

    def root_names():
        return set(c.getScriptName() for c in app.getChildren())

    def restore_root(before):
        """Destroys root nodes not in before; raises when one of before is missing."""
        now = root_names()
        for name in sorted(now - before):
            node = app.getNode(name)
            if node is not None:
                node.destroy(False)
        missing = before - root_names()
        if missing:
            raise RuntimeError("%d node(s) not restored, e.g. %s" % (len(missing), sorted(missing)[0]))

    def select(names):
        app.setSelection([app.getNode(n) for n in names])

    def middle(names, k):
        start = max(0, (len(names) - k) // 2)
        return names[start:start + k]

    def first_knob(name):
        node = app.getNode(name)
        for knob in gen_graph.EXPR_KNOBS.get(node.getPluginID(), ()):
            if node.getParam(knob) is not None:
                return knob
        return None

    def expression_of(name, knob):
        got = app.getNode(name).getParam(knob).getExpression(0)
        return str(got[0] if isinstance(got, (tuple, list)) else got)

    def set_expression(name, knob, expr):
        if not app.getNode(name).getParam(knob).setExpression(expr, False, 0):
            raise RuntimeError("could not set %s.%s = %r" % (name, knob, expr))

    class Ctx(object):
        def __init__(self, topology, n, summary, path):
            self.topology = topology
            self.n = n
            self.summary = summary
            self.path = path
            prefixes = set(gen_graph.KIND_PREFIX.values())
            self.processing = [nm for nm in summary["root_nodes"] if re.match(r"[A-Za-z]+", nm).group(0) in prefixes]
            self.filters = [nm for nm in self.processing if not nm.startswith("Merge")]
            self.viewer = None

    # ---- interaction ops -----------------------------------------------------------------------

    def op_load():
        def before(ctx, st, rep):
            clean_reset(ctx.path)

        def events(ctx, st, rep):
            if app.loadProject(ctx.path) is None:
                raise RuntimeError("loadProject(%s) failed" % ctx.path)
            yield None

        def teardown(ctx, st):
            if len(root_names()) < len(ctx.summary["root_nodes"]):
                raise RuntimeError("the reloaded project lacks nodes")
            gt.close_all_panels()
            activate()
            fit()
        return {"before": before, "events": events, "teardown": teardown}

    def op_fit_all():
        def events(ctx, st, rep):
            fit()
            yield None
        return {"events": events}

    def op_zoom_step():
        def events(ctx, st, rep):
            vp = root_graph().viewport()
            centre = QPointF(vp.width() / 2.0, vp.height() / 2.0)
            for steps in [1] * 10 + [-1] * 10:
                wheel(centre, steps)
                repaint()
                yield None

        def teardown(ctx, st):
            fit()
        return {"events": events, "teardown": teardown}

    def op_pan():
        def events(ctx, st, rep):
            vp = root_graph().viewport()
            x, y = vp.width() / 2.0, vp.height() / 2.0
            mouse(QEvent.MouseButtonPress, QPointF(x, y), Qt.MiddleButton, Qt.MiddleButton)
            repaint()
            yield None
            for i in range(1, DRAG_EVENTS - 1):
                mouse(QEvent.MouseMove, QPointF(x + 6 * i, y + 3 * i), Qt.NoButton, Qt.MiddleButton)
                repaint()
                yield None
            last = DRAG_EVENTS - 2
            mouse(QEvent.MouseButtonRelease, QPointF(x + 6 * last, y + 3 * last), Qt.MiddleButton, Qt.NoButton)
            repaint()
            yield None

        def after(ctx, st, rep):
            fit()
        return {"events": events, "after": after}

    def op_hover():
        def events(ctx, st, rep):
            vp = root_graph().viewport()
            w, h = max(1, vp.width() - 1), max(1, vp.height() - 1)
            for i in range(200):
                mouse(QEvent.MouseMove, QPointF((i * 37) % w, (i * 53) % h), Qt.NoButton, Qt.NoButton)
                repaint()
                yield None
        return {"events": events}

    def op_click_select():
        def setup(ctx, st):
            st["name"] = middle(ctx.processing, 1)[0]
            app.clearSelection()

        def events(ctx, st, rep):
            pos = to_view(*node_centre(st["name"]))
            mouse(QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
            repaint()
            yield None
            mouse(QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
            repaint()
            yield None

        def after(ctx, st, rep):
            app.clearSelection()
        return {"setup": setup, "events": events, "after": after}

    def op_select_all():
        def events(ctx, st, rep):
            app.selectAllNodes()
            repaint()
            yield None

        def after(ctx, st, rep):
            app.clearSelection()
        return {"events": events, "after": after}

    def op_rubber_band():
        def setup(ctx, st):
            boxes = []
            for name in ctx.summary["root_nodes"]:
                node = app.getNode(name)
                x, y = node.getPosition()
                w, h = node.getSize()
                boxes.append((x, y, x + w, y + h))
            x0 = min(b[0] for b in boxes)
            y0 = min(b[1] for b in boxes)
            x1 = max(b[2] for b in boxes)
            y1 = max(b[3] for b in boxes)
            # sqrt(0.1) of each side covers about a tenth of an evenly filled layout; the press
            # lands outside every node and backdrop.
            st["from"] = (x0 - 200.0, y0 - 200.0)
            st["to"] = (x0 + (x1 - x0) * 0.316, y0 + (y1 - y0) * 0.316)
            app.clearSelection()

        def events(ctx, st, rep):
            (sx, sy), (ex, ey) = st["from"], st["to"]
            mouse(QEvent.MouseButtonPress, to_view(sx, sy), Qt.LeftButton, Qt.LeftButton)
            repaint()
            yield None
            for i in range(1, DRAG_EVENTS - 1):
                f = i / float(DRAG_EVENTS - 2)
                mouse(QEvent.MouseMove, to_view(sx + (ex - sx) * f, sy + (ey - sy) * f), Qt.NoButton, Qt.LeftButton)
                repaint()
                yield None
            mouse(QEvent.MouseButtonRelease, to_view(ex, ey), Qt.LeftButton, Qt.NoButton)
            repaint()
            yield None

        def after(ctx, st, rep):
            st["selected"] = len(app.getSelectedNodes())
            app.clearSelection()

        def teardown(ctx, st):
            st["record"] = {"selected": st.get("selected", 0)}
        return {"setup": setup, "events": events, "after": after, "teardown": teardown}

    def op_drag_move(count):
        def setup(ctx, st):
            st["names"] = middle(ctx.processing, count)
            st["grab"] = st["names"][len(st["names"]) // 2]
            st["record"] = {"count": len(st["names"])}

        def before(ctx, st, rep):
            select(st["names"])
            repaint()

        def events(ctx, st, rep):
            start = to_view(*node_centre(st["grab"]))
            mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
            repaint()
            yield None
            for i in range(1, DRAG_EVENTS - 1):
                mouse(QEvent.MouseMove, QPointF(start.x() + 3 * i, start.y() + 2 * i), Qt.NoButton, Qt.LeftButton)
                repaint()
                yield None
            last = DRAG_EVENTS - 2
            mouse(QEvent.MouseButtonRelease, QPointF(start.x() + 3 * last, start.y() + 2 * last), Qt.LeftButton, Qt.NoButton)
            repaint()
            yield None

        def after(ctx, st, rep):
            trigger("undo")
            app.clearSelection()
        return {"setup": setup, "before": before, "events": events, "after": after}

    # ---- edit ops ------------------------------------------------------------------------------

    def op_create_one():
        def events(ctx, st, rep):
            node = app.createNode(gen_graph.GRADE)
            if node is None:
                raise RuntimeError("could not create a Grade")
            st["made"] = node.getScriptName()
            yield None

        def after(ctx, st, rep):
            node = app.getNode(st.pop("made", ""))
            if node is not None:
                node.destroy(False)
        return {"events": events, "after": after}

    def op_paste(count):
        def setup(ctx, st):
            st["names"] = middle(ctx.processing, min(count, ctx.n // 2))
            st["record"] = {"count": len(st["names"])}
            select(st["names"])
            invoke("copySelectedNodes")
            app.clearSelection()
            st["before"] = root_names()

        def events(ctx, st, rep):
            invoke("pasteNodeClipBoards")
            yield None

        def after(ctx, st, rep):
            trigger("undo")
            app.clearSelection()
            restore_root(st["before"])
        return {"setup": setup, "events": events, "after": after}

    def op_duplicate(count):
        def setup(ctx, st):
            st["names"] = middle(ctx.processing, min(count, ctx.n // 2))
            st["record"] = {"count": len(st["names"])}
            st["before"] = root_names()

        def before(ctx, st, rep):
            select(st["names"])

        def events(ctx, st, rep):
            invoke("duplicateSelectedNodes")
            yield None

        def after(ctx, st, rep):
            trigger("undo")
            app.clearSelection()
            restore_root(st["before"])
        return {"setup": setup, "before": before, "events": events, "after": after}

    def delete_setup(count):
        def setup(ctx, st):
            st["names"] = middle(ctx.processing, min(count, ctx.n // 2))
            st["record"] = {"count": len(st["names"])}
            st["before"] = root_names()
        return setup

    def op_delete(count):
        def before(ctx, st, rep):
            select(st["names"])

        def events(ctx, st, rep):
            invoke("deleteSelection")
            yield None

        def after(ctx, st, rep):
            trigger("undo")
            app.clearSelection()
            restore_root(st["before"])
        return {"setup": delete_setup(count), "before": before, "events": events, "after": after}

    def op_undo(count):
        def before(ctx, st, rep):
            select(st["names"])
            invoke("deleteSelection")

        def events(ctx, st, rep):
            trigger("undo")
            yield None

        def after(ctx, st, rep):
            app.clearSelection()
            restore_root(st["before"])
        return {"setup": delete_setup(count), "before": before, "events": events, "after": after}

    def op_redo(count):
        def before(ctx, st, rep):
            select(st["names"])
            invoke("deleteSelection")
            trigger("undo")

        def events(ctx, st, rep):
            trigger("redo")
            yield None

        def after(ctx, st, rep):
            trigger("undo")
            app.clearSelection()
            restore_root(st["before"])
        return {"setup": delete_setup(count), "before": before, "events": events, "after": after}

    def op_set_expression():
        def setup(ctx, st):
            names = [nm for nm in ctx.processing if first_knob(nm)]
            mid = len(names) // 2
            st["target"] = names[mid]
            st["knob"] = first_knob(names[mid])
            st["ref"] = names[max(0, mid - 10)]
            st["ref_knob"] = first_knob(st["ref"])
            st["prev"] = expression_of(st["target"], st["knob"])

        def events(ctx, st, rep):
            set_expression(st["target"], st["knob"], "%s.%s.getValue(0) * 0.25 + %d" % (st["ref"], st["ref_knob"], rep))
            yield None

        def after(ctx, st, rep):
            set_expression(st["target"], st["knob"], st["prev"])
        return {"setup": setup, "events": events, "after": after}

    def op_edit_label():
        def setup(ctx, st):
            labelled = [nm for nm in ctx.summary["label_nodes"] if "." not in nm]
            st["target"] = middle(labelled, 1)[0]
            st["prev"] = str(app.getNode(st["target"]).getParam(gen_graph.LABEL_KNOB).get())

        def events(ctx, st, rep):
            html = "<br>".join(["<b>%s</b> revision %d" % (st["target"], rep),
                                "<font color=\"#80c0ff\">approved for comp</font>",
                                "<i>matched to plate v%d</i>" % (rep + 2),
                                "<u>todo</u> despill edges",
                                "owner: bench"])
            app.getNode(st["target"]).getParam(gen_graph.LABEL_KNOB).set(html)
            yield None

        def after(ctx, st, rep):
            app.getNode(st["target"]).getParam(gen_graph.LABEL_KNOB).set(st["prev"])
        return {"setup": setup, "events": events, "after": after}

    def op_knob_change_fanout():
        def setup(ctx, st):
            hubs = [nm for nm in ctx.processing if app.getNode(nm).getPluginID() == gen_graph.TRANSFORM]
            if not hubs:
                raise RuntimeError("no Transform to drive the fan-out")
            hub = middle(hubs, 1)[0]
            st["hub"] = hub
            st["hub_value"] = app.getNode(hub).getParam("rotate").get()
            others = [nm for nm in ctx.processing if nm != hub and first_knob(nm)]
            st["dependents"] = []
            for nm in middle(others, 50):
                knob = first_knob(nm)
                st["dependents"].append((nm, knob, expression_of(nm, knob)))
                set_expression(nm, knob, "%s.rotate.getValue(0) * 0.01 + 1" % hub)
            st["record"] = {"count": len(st["dependents"])}

        def events(ctx, st, rep):
            app.getNode(st["hub"]).getParam("rotate").set(1.5 + rep)
            yield None

        def teardown(ctx, st):
            for nm, knob, prev in st.get("dependents", []):
                set_expression(nm, knob, prev)
            if "hub_value" in st:
                app.getNode(st["hub"]).getParam("rotate").set(st["hub_value"])
        return {"setup": setup, "events": events, "teardown": teardown}

    def op_connect():
        def setup(ctx, st):
            names = [nm for nm in ctx.filters if app.getNode(nm).getInput(0) is not None]
            mid = len(names) // 2
            st["target"] = names[mid]
            st["orig"] = app.getNode(names[mid]).getInput(0).getScriptName()
            st["alt"] = names[max(0, mid - 10)]
            if st["alt"] in (st["target"], st["orig"]):
                st["alt"] = names[0]

        def events(ctx, st, rep):
            if not app.getNode(st["target"]).connectInput(0, app.getNode(st["alt"])):
                raise RuntimeError("could not rewire %s" % st["target"])
            yield None

        def after(ctx, st, rep):
            app.getNode(st["target"]).connectInput(0, app.getNode(st["orig"]))
        return {"setup": setup, "events": events, "after": after}

    def op_frame_step():
        def setup(ctx, st):
            viewer = app.getNode("BenchViewer")
            if viewer is None:
                props = {"CreateNodeArgsPropNodeInitialName": NatronEngine.StringNodeCreationProperty("BenchViewer")}
                viewer = app.createNode("fr.inria.built-in.Viewer", -1, None, props)
            if viewer is None:
                raise RuntimeError("could not create a Viewer")
            st["created"] = True
            st["viewer"] = app.getViewer("BenchViewer")
            if st["viewer"] is None:
                raise RuntimeError("the Viewer has no Python wrapper")
            st["viewer"].seek(1)

        def events(ctx, st, rep):
            for frame in range(2, 12):
                st["viewer"].seek(frame)
                yield None

        def after(ctx, st, rep):
            st["viewer"].seek(1)

        def teardown(ctx, st):
            viewer = app.getNode("BenchViewer")
            if viewer is not None:
                viewer.destroy(False)
        return {"setup": setup, "events": events, "after": after, "teardown": teardown}

    def op_idle():
        def events(ctx, st, rep):
            c0 = time.process_time()
            yield ("sleep", int(IDLE_S * 1000))
            st.setdefault("cpu", []).append((time.process_time() - c0) * 1000.0)

        def teardown(ctx, st):
            cpu = st.get("cpu", [])
            st["record"] = {"cpu_ms": round(statistics.median(cpu), 3) if cpu else None}
        return {"events": events, "teardown": teardown}

    def op_self_check():
        def events(ctx, st, rep):
            time.sleep(0.3)
            yield None
        return {"events": events, "reps": 1}

    INTERACTION_OPS = [
        ("load", op_load), ("fit_all", op_fit_all), ("zoom_step", op_zoom_step), ("pan", op_pan),
        ("hover", op_hover), ("click_select", op_click_select), ("select_all", op_select_all),
        ("rubber_band", op_rubber_band), ("drag_move_1", lambda: op_drag_move(1)),
        ("drag_move_50", lambda: op_drag_move(50)),
    ]
    EDIT_OPS = [
        ("create_one", op_create_one), ("paste_100", lambda: op_paste(100)), ("paste_1000", lambda: op_paste(1000)),
        ("duplicate_100", lambda: op_duplicate(100)), ("delete_1000", lambda: op_delete(1000)),
        ("undo", lambda: op_undo(1000)), ("redo", lambda: op_redo(1000)), ("set_expression", op_set_expression),
        ("edit_label", op_edit_label), ("knob_change_fanout", op_knob_change_fanout), ("connect", op_connect),
        ("frame_step", op_frame_step), ("idle", op_idle),
    ]

    def selected(name):
        return not ONLY_OPS or name in ONLY_OPS

    # ---- driver --------------------------------------------------------------------------------

    def p95(values):
        ordered = sorted(values)
        return ordered[max(0, int(math.ceil(0.95 * len(ordered))) - 1)]

    def short_error():
        return traceback.format_exc().strip().splitlines()[-1]

    def run_op(name, make, ctx, kind):
        op = make()
        reps = op.get("reps", REPS)
        st = {}
        times, windows = [], []
        event_max = 0.0
        events_per_rep = 0
        status, error = "ok", None
        deadline = time.monotonic() + OP_TIMEOUT_S
        try:
            if "setup" in op:
                op["setup"](ctx, st)
            yield ("sleep", 0)
            for rep in range(reps):
                if "before" in op:
                    op["before"](ctx, st, rep)
                yield ("sleep", 0)
                gen = op["events"](ctx, st, rep)
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
                    op["after"](ctx, st, rep)
                if status != "ok":
                    break
        except Exception:
            status, error = "error", short_error()
        try:
            if "teardown" in op:
                op["teardown"](ctx, st)
        except Exception:
            if status == "ok":
                status, error = "error", "teardown: " + short_error()
        yield ("sleep", STALL_FLUSH_MS)
        record = {
            "type": "op", "op": name, "kind": kind, "topology": ctx.topology, "n": ctx.n,
            "status": status, "reps": len(times), "events_per_rep": events_per_rep,
            "per_event_max_ms": round(event_max, 3),
            "stalls": attribute_stalls(windows),
        }
        if times:
            record.update({"median_ms": round(statistics.median(times), 3), "p95_ms": round(p95(times), 3),
                           "min_ms": round(min(times), 3), "max_ms": round(max(times), 3)})
        if error:
            record["error"] = error
        record.update(st.get("record", {}))
        emit(record)
        report("RESULT " + json.dumps(record, sort_keys=True))
        check(status == "ok", "%s %s n=%d: %s%s" % (name, ctx.topology, ctx.n, status, (" (" + error + ")") if error else ""))
        if name == "self_check":
            check(record["stalls"]["count"] == 1 and record["stalls"]["max_ms"] >= 250.0,
                  "self_check %s n=%d: the 300 ms block is attributed one stall (%s)" % (ctx.topology, ctx.n, record["stalls"]))
        if name == "idle":
            check((record.get("cpu_ms") or 0) > 0, "idle %s n=%d: CPU time is reported (%s ms)" % (ctx.topology, ctx.n, record.get("cpu_ms")))

    def clean_reset(path):
        if path and not app.saveProject(path):
            raise RuntimeError("could not save %s before resetting" % path)
        if not app.resetProject():
            raise RuntimeError("resetProject failed")

    def bench_graph(topology, n, workdir, previous_path):
        if previous_path is not None:
            clean_reset(previous_path)
            yield ("sleep", SETTLE_MS)
        gt.section("%s-%d" % (topology, n))
        t0 = mono_ms()
        summary = gen_graph.build(app, topology, n, SEED)
        gen_ms = mono_ms() - t0
        graph = dict((k, summary[k]) for k in gen_graph.SUMMARY_KEYS)
        graph.update({"type": "graph", "seed": SEED, "gen_ms": round(gen_ms, 3)})
        emit(graph)
        report("GRAPH " + json.dumps(graph, sort_keys=True))
        gt.require(summary["nodes"] == n, "%s n=%d: generated %d processing nodes" % (topology, n, summary["nodes"]))
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        path = os.path.join(workdir, "%s-%d.ntpz" % (topology, n))
        gt.require(app.saveProjectAs(path), "%s n=%d: saved to %s" % (topology, n, path))
        ctx = Ctx(topology, n, summary, path)
        activate()
        fit()
        yield ("sleep", SETTLE_MS)
        for name, make in INTERACTION_OPS:
            if selected(name):
                for cmd in run_op(name, make, ctx, "interaction"):
                    yield cmd
        before = root_names()
        ran_edit = False
        for name, make in EDIT_OPS:
            if selected(name):
                ran_edit = True
                for cmd in run_op(name, make, ctx, "edit"):
                    yield cmd
        if ran_edit:
            after = root_names()
            check(after == before, "%s n=%d: the edit ops leave the root graph as they found it (%d -> %d nodes)" %
                  (topology, n, len(before), len(after)))
        if SELF_CHECK:
            for cmd in run_op("self_check", op_self_check, ctx, "check"):
                yield cmd
        return path

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the node graph is up")
        gt.close_all_panels()
        if not STALL_LOG:
            report("NOTE NATRON_STALL_LOG is not set; no stalls will be attributed")
        emit(host_record())
        workdir = tempfile.mkdtemp(prefix="gui-bench-")
        previous = None
        for topology in TOPOLOGIES:
            for n in SIZES:
                gen = bench_graph(topology, n, workdir, previous)
                try:
                    while True:
                        cmd = next(gen)
                        yield cmd
                except StopIteration as stop:
                    previous = stop.value
        _out.close()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
