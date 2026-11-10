"""GUI benchmark: times node graph interaction and edit operations on generated large graphs.

Runs inside the Natron GUI, normally through tools/bench/gui/run.sh, which wraps
Tests/gui/run-gui-test.sh (Xvfb) or runs an AppImage on the host display:

    Tests/gui/run-gui-test.sh tools/bench/gui/gui_bench.py [natron-binary]

Configured by the environment:
    GUI_BENCH_TOPOLOGIES  comma-separated gen_graph topologies (default mixed)
    GUI_BENCH_SIZES       comma-separated node counts (default 300)
    GUI_BENCH_REPS        repetitions per op (default 5)
    GUI_BENCH_OPS         comma-separated op names to run (default all)
    GUI_BENCH_OP_TIMEOUT_S  per-op budget; an op past it is recorded as "timeout" (default 120). A single
                          step that does not return within 2x that plus 60 s is recorded as a "wedged"
                          timeout and the process exits with code 3, which run.sh answers by
                          restarting Natron with GUI_BENCH_RESUME=1
    GUI_BENCH_RESUME      1 skips every (topology, n, op) already recorded in GUI_BENCH_OUT, and sizes
                          with nothing left to run
    GUI_BENCH_SKIP_WEDGED 1 (default) records an op that wedged Natron at a smaller n as a timeout at the
                          larger sizes without running it
    GUI_BENCH_HOLD_FILE   when set, each op reports "BEGIN op topology n" after its setup and waits up to
                          GUI_BENCH_HOLD_S (default 120) for this file to exist before its reps, so a
                          stack sampler can attach first; the file is removed once seen
    GUI_BENCH_PTRACER_ANY 1 lets any process of this user attach to Natron (prctl PR_SET_PTRACER_ANY),
                          for a stack sampler under kernel.yama.ptrace_scope 1
    GUI_BENCH_SEED        generator seed (default 1)
    GUI_BENCH_IDLE_S      length of the idle op (default 5)
    GUI_BENCH_FORCED_REPAINT  0 replaces each interaction event's forced viewport repaint with a
                          processEvents pass (default 1)
    GUI_BENCH_SELF_CHECK  1 adds an op that blocks the main thread for 300 ms once and checks
                          that the stall log attributes exactly one stall to it
    GUI_BENCH_OUT         JSON-lines output (default $NATRON_GUI_TEST_OUT/gui_bench.jsonl)
    GUI_BENCH_MODE, GUI_BENCH_BUILD_TYPE, GUI_BENCH_GIT_SHA (the harness checkout), GUI_BENCH_APPIMAGE,
    GUI_BENCH_APPIMAGE_NAME, GUI_BENCH_APPIMAGE_BYTES, GUI_BENCH_APPIMAGE_MTIME, GUI_BENCH_APPIMAGE_SHA256
                          copied into the host record
    NATRON_STALL_LOG      the stall monitor's log; stalls are attributed to the op whose timed
                          window holds their midpoint

Output: one host record, then per (topology, n) one graph record and one record per op with the
median, p95, min and max of the per-rep wall times, the longest single event, and the stalls.
Each op also reports, over all its events, the median and max of the timed step's wall time
(step_wall_*), its main-thread CPU time (step_cpu_*) and the event-loop pass that follows it
(loop_pass_*), which the per-rep wall time folds together.
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
import threading
import time
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))
sys.path.insert(0, os.path.join(ROOT, "tools", "bench", "gui"))

STARTUP_TIMEOUT_S = 120.0
SETTLE_MS = 300
STALL_FLUSH_MS = 120
DRAG_EVENTS = 40
RUBBER_BAND_PRESS_MARGIN = 40.0
DIALOG_POLL_MS = 100
DIALOG_ACCEPT = ("Yes", "OK", "Continue")
DIALOG_TEXT_CHARS = 500


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
FORCED_REPAINT = os.environ.get("GUI_BENCH_FORCED_REPAINT", "1") != "0"
STALL_LOG = os.environ.get("NATRON_STALL_LOG", "")
WATCHDOG_S = int(os.environ.get("GUI_BENCH_WATCHDOG_S", str(6 * 3600)))
RESUME = os.environ.get("GUI_BENCH_RESUME", "0") == "1"
SKIP_WEDGED = os.environ.get("GUI_BENCH_SKIP_WEDGED", "1") != "0"
HOLD_FILE = os.environ.get("GUI_BENCH_HOLD_FILE", "")
HOLD_S = float(os.environ.get("GUI_BENCH_HOLD_S", "120"))
WEDGED_EXIT = 3

try:
    import NatronEngine
    from PySide6.QtCore import Q_ARG, QEvent, QMetaObject, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QGuiApplication, QMouseEvent, QWheelEvent
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLabel, QMainWindow, QTextEdit, QWidget

    import gen_graph
    import guitest as gt
    from guitest import check, report

    if os.environ.get("GUI_BENCH_PTRACER_ANY") == "1":
        # Under kernel.yama.ptrace_scope 1 only an ancestor may attach, and the stack sampler is not one.
        import ctypes
        _libc = ctypes.CDLL(None, use_errno=True)
        _libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
        PR_SET_PTRACER, PR_SET_PTRACER_ANY = 0x59616d61, ctypes.c_ulong(-1).value
        if _libc.prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0:
            report("NOTE prctl(PR_SET_PTRACER_ANY) failed: %s" % os.strerror(ctypes.get_errno()))

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

    # A single event that never returns can't be caught between events. This thread records the op
    # as a timeout and ends the process once one step, together with the event loop pass that follows
    # it, has run well past the op's budget. The guard is re-armed before every step, so an op that is
    # slow overall but keeps returning is left to the per-op budget instead.
    _guard = {"until": None, "record": None, "phase": None}
    _dialogs = []

    def arm_guard(phase):
        _guard["phase"] = phase
        _guard["until"] = time.monotonic() + GUARD_S

    def _guard_loop():
        while True:
            time.sleep(1.0)
            until = _guard["until"]
            if until is not None and time.monotonic() > until:
                record = dict(_guard["record"])
                record.update({"status": "timeout", "wedged": True, "timeout_s": GUARD_S, "phase": _guard["phase"],
                               "error": "one %s step of the op did not return within %.0f s" % (_guard["phase"], GUARD_S)})
                if _dialogs:
                    record["dialogs"] = list(_dialogs)
                emit(record)
                _out.close()
                report("RESULT " + json.dumps(record, sort_keys=True))
                report("FAIL [%s-%d] %s wedged Natron in its %s step (> %.0f s); exiting so the run restarts" % (
                    record["topology"], record["n"], record["op"], _guard["phase"], GUARD_S))
                os._exit(WEDGED_EXIT)

    GUARD_S = OP_TIMEOUT_S * 2 + 60

    def recorded_ops():
        """({(topology, n, op)} already in OUT_PATH, {(topology, op): (n, timeout_s)} of the smallest n each op wedged at)."""
        done, wedged = set(), {}
        if not os.path.isfile(OUT_PATH):
            return done, wedged
        with open(OUT_PATH) as f:
            for line in f:
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue
                if rec.get("type") != "op":
                    continue
                done.add((rec["topology"], rec["n"], rec["op"]))
                if rec.get("wedged") and not rec.get("not_run"):
                    key = (rec["topology"], rec["op"])
                    if key not in wedged or rec["n"] < wedged[key][0]:
                        wedged[key] = (rec["n"], rec.get("timeout_s", GUARD_S))
        return done, wedged

    DONE, WEDGED = recorded_ops() if RESUME else (set(), {})
    threading.Thread(target=_guard_loop, daemon=True).start()

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
            "appimage_name": os.environ.get("GUI_BENCH_APPIMAGE_NAME") or None,
            "appimage_bytes": int(os.environ.get("GUI_BENCH_APPIMAGE_BYTES") or 0) or None,
            "appimage_mtime": os.environ.get("GUI_BENCH_APPIMAGE_MTIME") or None,
            "appimage_sha256": os.environ.get("GUI_BENCH_APPIMAGE_SHA256") or None,
            "resumed": RESUME,
            "natron_version": NatronEngine.natron.getNatronVersionString(),
            "stall_log": bool(STALL_LOG),
            "reps": REPS,
            "forced_repaint": FORCED_REPAINT,
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

    root_graph_cache = [None]

    def root_graph():
        """The project's graph. A group's graph is named after the group; the root's has no name.
        Kept once found: walking every widget from Python on each synthetic event would swamp
        what the hover op measures."""
        graph = root_graph_cache[0]
        if graph is not None:
            try:
                graph.objectName()
                return graph
            except RuntimeError:
                pass
        graphs = [w for w in gt.widgets_of("NodeGraph") if not w.objectName()]
        root_graph_cache[0] = graphs[0] if graphs else None
        return root_graph_cache[0]

    def front_root_graph():
        """Brings the project's graph to the front of its pane. Once a graph has had focus, creating
        a Group opens the group's graph as a new current tab, so from the second graph of a run on
        the generator leaves the last group in front of the root."""
        graph = root_graph()
        pane = graph.parentWidget()
        bars = [w for w in pane.findChildren(QWidget) if gt.class_name(w) == "TabBar"]
        if not graph.isVisible() and len(bars) == 1:
            for index in range(bars[0].property("count")):
                QMetaObject.invokeMethod(bars[0], "setCurrentIndex", Q_ARG(int, index))
                QApplication.processEvents()
                if graph.isVisible():
                    break
        gt.require(graph.isVisible(), "the project's node graph is in front of its pane")

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
        # Gui calls that take no graph act on the last focused one, which can be a Group's.
        graph = root_graph()
        if graph is not None and not graph.hasFocus():
            graph.setFocus(Qt.OtherFocusReason)
            QApplication.processEvents()

    # Passing app pins these to the project's graph: without a group they read the last focused
    # graph, which can be a Group's even while every click lands in the project's.
    def selected_nodes():
        return app.getSelectedNodes(app)

    def clear_selection():
        app.clearSelection(app)

    def invoke(slot):
        QMetaObject.invokeMethod(root_graph(), slot)

    def dialog_text(dialog):
        parts = [w.text() for w in dialog.findChildren(QLabel) if w.text()]
        parts += [w.toPlainText() for w in dialog.findChildren(QTextEdit) if w.toPlainText()]
        return " | ".join(" ".join(p.split()) for p in parts)[:DIALOG_TEXT_CHARS]

    def answer_dialog():
        """Answers the active modal dialog with Yes or OK when it offers one, else rejects it, and
        notes it for the running op's record. Returns the answer, or None when no dialog is up."""
        dialog = QApplication.activeModalWidget()
        if dialog is None:
            return None
        for button in dialog.findChildren(QAbstractButton):
            label = button.text().replace("&", "")
            if label in DIALOG_ACCEPT and button.isVisible() and button.isEnabled():
                break
        else:
            button, label = None, "rejected"
        seen = {"title": dialog.windowTitle(), "text": dialog_text(dialog), "answer": label, "phase": _guard["phase"]}
        _dialogs.append(seen)
        where = "%s (%s step)" % (_guard["record"]["op"], seen["phase"]) if seen["phase"] else "no op"
        report("NOTE dialog during %s answered %s: %s: %s" % (where, label, seen["title"], seen["text"]))
        if button is not None:
            button.click()
        elif hasattr(dialog, "reject"):
            dialog.reject()
        else:
            dialog.close()
        return label

    # A modal dialog runs its own event loop, so an unanswered one blocks the driver until the guard
    # gives up on the op; this answers any that appears while the driver waits on Natron.
    _dialog_timer = QTimer()
    _dialog_timer.setInterval(DIALOG_POLL_MS)
    _dialog_timer.timeout.connect(answer_dialog)
    _dialog_timer.start()

    def delete_selection():
        """Deletes the selection, answering every prompt it raises; returns how many there were."""
        before = len(_dialogs)
        # deleteSelection asks before breaking a link or expression that a surviving node reads, and
        # polled this fast the answer adds next to nothing to the timed delete.
        timer = QTimer()
        timer.setInterval(1)
        timer.timeout.connect(answer_dialog)
        timer.start()
        try:
            invoke("deleteSelection")
        finally:
            timer.stop()
        return len(_dialogs) - before

    def repaint():
        if FORCED_REPAINT:
            root_graph().viewport().repaint()
        else:
            QApplication.processEvents()

    root_offset = [0.0, 0.0]

    def fit():
        clear_selection()
        invoke("centerOnAllNodes")
        repaint()
        # Framing moves the graph's root item so the top-left node sits at the scene origin.
        positions = [c.getPosition() for c in app.getChildren()]
        if positions:
            root_offset[0] = -min(p[0] for p in positions)
            root_offset[1] = -min(p[1] for p in positions)

    def to_view(x, y):
        graph = root_graph()
        p = graph.mapFromScene(QPointF(x + root_offset[0], y + root_offset[1]))
        return QPointF(p.x(), p.y())

    def frame_on(name):
        """Centres the graph on the node at scale 1, so a press at its centre cannot miss it."""
        activate()
        fit()
        graph = root_graph()
        x, y = node_centre(name)
        graph.resetTransform()
        graph.centerOn(x + root_offset[0], y + root_offset[1])
        QApplication.processEvents()
        return to_view(x, y)

    def find_clickable(names, fitted=False):
        """A node a press at its centre selects, at scale 1 or, when fitted, with the whole graph framed.
        Backdrops can cover a node, and zoomed out a neighbour can take the press, so each candidate is tried."""
        order = sorted(range(len(names)), key=lambda i: abs(i - len(names) // 2))
        for i in order[:25]:
            name = names[i]
            if fitted:
                activate()
                fit()
                pos = to_view(*node_centre(name))
            else:
                pos = frame_on(name)
            mouse(QEvent.MouseMove, pos, Qt.NoButton, Qt.NoButton)
            QApplication.processEvents()
            mouse(QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
            QApplication.processEvents()
            mouse(QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
            QApplication.processEvents()
            ok = name in [n.getScriptName() for n in selected_nodes()]
            clear_selection()
            if ok:
                return name
        raise RuntimeError("no node near the middle of the graph can be selected by a click%s" %
                           (" with the graph fitted" if fitted else ""))

    def node_centre(name):
        """Where a press grabs the node: its left edge, away from the input and output edges that run
        through its middle and take the press when it lands near them."""
        node = app.getNode(name)
        x, y = node.getPosition()
        w, h = node.getSize()
        return x + 8.0, y + h / 2.0

    def mouse(etype, pos, button, buttons, modifiers=Qt.NoModifier):
        graph = root_graph()
        QApplication.sendEvent(graph.viewport(), QMouseEvent(etype, pos, button, buttons, modifiers))

    def wheel(pos, steps):
        graph = root_graph()
        vp = graph.viewport()
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
            graph = root_graph()
            vp = graph.viewport()
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
            graph = root_graph()
            vp = graph.viewport()
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
            graph = root_graph()
            vp = graph.viewport()
            w, h = max(1, vp.width() - 1), max(1, vp.height() - 1)
            for i in range(200):
                mouse(QEvent.MouseMove, QPointF((i * 37) % w, (i * 53) % h), Qt.NoButton, Qt.NoButton)
                repaint()
                yield None
        return {"events": events}

    def op_click_select():
        def setup(ctx, st):
            st["name"] = find_clickable(ctx.processing)
            fit()

        def before(ctx, st, rep):
            st["pos"] = frame_on(st["name"])

        def events(ctx, st, rep):
            pos = st["pos"]
            mouse(QEvent.MouseMove, pos, Qt.NoButton, Qt.NoButton)
            QApplication.processEvents()
            mouse(QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
            repaint()
            yield None
            mouse(QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
            repaint()
            yield None

        def after(ctx, st, rep):
            hit = [n.getScriptName() for n in selected_nodes()]
            fit()
            if st["name"] not in hit:
                raise RuntimeError("click did not select %s (selected %d)" % (st["name"], len(hit)))
        return {"setup": setup, "before": before, "events": events, "after": after}

    def op_select_all():
        def events(ctx, st, rep):
            app.selectAllNodes(app)
            repaint()
            yield None

        def after(ctx, st, rep):
            selected = len(selected_nodes())
            clear_selection()
            if selected < len(ctx.processing):
                raise RuntimeError("select all selected %d of %d nodes" % (selected, len(ctx.processing)))
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
            # sqrt(0.1) of each side covers about a tenth of an evenly filled layout.
            st["corner"] = (x0, y0)
            st["to"] = (x0 + (x1 - x0) * 0.316, y0 + (y1 - y0) * 0.316)
            clear_selection()

        def before(ctx, st, rep):
            # The press misses every node, backdrop and edge only if it clears their grab radii,
            # which are in viewport pixels: a fixed scene distance falls inside them zoomed out.
            corner = to_view(*st["corner"])
            st["start"] = QPointF(corner.x() - RUBBER_BAND_PRESS_MARGIN, corner.y() - RUBBER_BAND_PRESS_MARGIN)
            st["end"] = to_view(*st["to"])

        def events(ctx, st, rep):
            start, end = st["start"], st["end"]
            mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
            repaint()
            yield None
            for i in range(1, DRAG_EVENTS - 1):
                f = i / float(DRAG_EVENTS - 2)
                at = QPointF(start.x() + (end.x() - start.x()) * f, start.y() + (end.y() - start.y()) * f)
                mouse(QEvent.MouseMove, at, Qt.NoButton, Qt.LeftButton)
                repaint()
                yield None
            mouse(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
            repaint()
            yield None

        def after(ctx, st, rep):
            st["selected"] = len(selected_nodes())
            clear_selection()
            if not st["selected"]:
                raise RuntimeError("the rubber band selected nothing")

        def teardown(ctx, st):
            st["record"] = {"selected": st.get("selected", 0)}
        return {"setup": setup, "before": before, "events": events, "after": after, "teardown": teardown}

    def op_drag_move(count):
        def setup(ctx, st):
            if count == 1:
                st["names"] = [find_clickable(ctx.processing)]
                st["grab"] = st["names"][0]
            else:
                st["names"] = middle(ctx.processing, count)
                st["grab"] = find_clickable(st["names"], fitted=True)
            st["record"] = {"count": len(st["names"])}

        def before(ctx, st, rep):
            fit()
            select(st["names"])
            repaint()
            st["start"] = frame_on(st["grab"]) if len(st["names"]) == 1 else to_view(*node_centre(st["grab"]))
            st["pos0"] = app.getNode(st["grab"]).getPosition()

        def events(ctx, st, rep):
            start = st["start"]
            mouse(QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
            QApplication.processEvents()
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
            pos1 = app.getNode(st["grab"]).getPosition()
            if pos1 == st["pos0"]:
                graph = root_graph()
                pt = to_view(*node_centre(st["grab"]))
                item = graph.itemAt(int(pt.x()), int(pt.y()))
                raise RuntimeError("drag did not move %s (at %s, press at view %s, viewport %s, item %s, selected %d)" % (
                    st["grab"], st["pos0"], (pt.x(), pt.y()), graph.viewport().size(), item, len(selected_nodes())))
            trigger("undo")
            clear_selection()
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
            clear_selection()
            st["before"] = root_names()

        def events(ctx, st, rep):
            invoke("pasteNodeClipBoards")
            yield None

        def after(ctx, st, rep):
            clear_selection()
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
            clear_selection()
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
            st["record"]["prompts"] = st["record"].get("prompts", 0) + delete_selection()
            yield None

        def after(ctx, st, rep):
            trigger("undo")
            clear_selection()
            restore_root(st["before"])
        return {"setup": delete_setup(count), "before": before, "events": events, "after": after}

    def op_undo(count):
        def before(ctx, st, rep):
            select(st["names"])
            st["record"]["prompts"] = st["record"].get("prompts", 0) + delete_selection()

        def events(ctx, st, rep):
            trigger("undo")
            yield None

        def after(ctx, st, rep):
            clear_selection()
            restore_root(st["before"])
        return {"setup": delete_setup(count), "before": before, "events": events, "after": after}

    def op_redo(count):
        def before(ctx, st, rep):
            select(st["names"])
            st["record"]["prompts"] = st["record"].get("prompts", 0) + delete_selection()
            trigger("undo")

        def events(ctx, st, rep):
            trigger("redo")
            yield None

        def after(ctx, st, rep):
            trigger("undo")
            clear_selection()
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
            target = app.getNode(st["target"])
            target.disconnectInput(0)
            if not target.connectInput(0, app.getNode(st["alt"])):
                raise RuntimeError("could not rewire %s" % st["target"])
            yield None

        def after(ctx, st, rep):
            target = app.getNode(st["target"])
            target.disconnectInput(0)
            if not target.connectInput(0, app.getNode(st["orig"])):
                raise RuntimeError("could not restore %s" % st["target"])
        return {"setup": setup, "events": events, "after": after}

    def op_frame_step():
        def setup(ctx, st):
            viewer = app.getNode("BenchViewer")
            if viewer is None:
                props = {"CreateNodeArgsPropNodeInitialName": NatronEngine.StringNodeCreationProperty("BenchViewer")}
                viewer = app.createNode("Viewer", -1, None, props)
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
        ("fit_all", op_fit_all), ("zoom_step", op_zoom_step), ("pan", op_pan),
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
        step_wall, step_cpu, loop_pass = [], [], []
        events_per_rep = 0
        status, error = "ok", None
        del _dialogs[:]
        _guard["record"] = {"type": "op", "op": name, "kind": kind, "topology": ctx.topology, "n": ctx.n,
                            "reps": 0, "stalls": {"count": 0, "max_ms": 0.0, "total_ms": 0.0}}
        arm_guard("setup")
        try:
            if "setup" in op:
                op["setup"](ctx, st)
            yield ("sleep", 0)
            if HOLD_FILE:
                _guard["until"] = None
                report("BEGIN %s %s %d" % (name, ctx.topology, ctx.n))
                yield ("until", lambda: os.path.exists(HOLD_FILE), HOLD_S)
                if os.path.exists(HOLD_FILE):
                    os.remove(HOLD_FILE)
                else:
                    report("NOTE wait expired: %s was not released by GUI_BENCH_HOLD_FILE within %.0f s; running without a sampler" % (name, HOLD_S))
            deadline = time.monotonic() + OP_TIMEOUT_S
            for rep in range(reps):
                arm_guard("before")
                if "before" in op:
                    op["before"](ctx, st, rep)
                yield ("sleep", 0)
                gen = op["events"](ctx, st, rep)
                count = 0
                t0 = mono_ms()
                step_end = None
                while True:
                    e0 = mono_ms()
                    if step_end is not None:
                        loop_pass.append(e0 - step_end)
                    c0 = time.thread_time()
                    arm_guard("events")
                    try:
                        cmd = next(gen)
                        done = False
                    except StopIteration:
                        done = True
                    step_end = mono_ms()
                    event_max = max(event_max, step_end - e0)
                    if done:
                        break
                    step_wall.append(step_end - e0)
                    step_cpu.append((time.thread_time() - c0) * 1000.0)
                    if cmd and tuple(cmd) != ("sleep", 0):
                        step_end = None
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
                arm_guard("after")
                if "after" in op:
                    op["after"](ctx, st, rep)
                if status != "ok":
                    break
        except Exception:
            status, error = "error", short_error()
        arm_guard("teardown")
        try:
            if "teardown" in op:
                op["teardown"](ctx, st)
        except Exception:
            if status == "ok":
                status, error = "error", "teardown: " + short_error()
        _guard["until"] = None
        _guard["phase"] = None
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
        for field, values in (("step_wall", step_wall), ("step_cpu", step_cpu), ("loop_pass", loop_pass)):
            if values:
                record[field + "_median_ms"] = round(statistics.median(values), 3)
                record[field + "_max_ms"] = round(max(values), 3)
        if error:
            record["error"] = error
        if status == "timeout":
            record["timeout_s"] = OP_TIMEOUT_S
        record.update(st.get("record", {}))
        if _dialogs:
            record["dialogs"] = list(_dialogs)
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

    def op_plan(topology, n):
        """[(name, make, kind)] in run order, less the ops a resumed run has already recorded."""
        plan = [(name, make, "interaction") for name, make in INTERACTION_OPS if selected(name)]
        plan += [(name, make, "edit") for name, make in EDIT_OPS if selected(name)]
        if selected("load"):
            plan.append(("load", op_load, "interaction"))
        if SELF_CHECK:
            plan.append(("self_check", op_self_check, "check"))
        return [p for p in plan if (topology, n, p[0]) not in DONE]

    def record_not_run(name, kind, topology, n):
        """Records name as a timeout without running it when it wedged Natron at a smaller n; True if it did."""
        hit = WEDGED.get((topology, name)) if SKIP_WEDGED else None
        if hit is None or hit[0] >= n:
            return False
        record = {"type": "op", "op": name, "kind": kind, "topology": topology, "n": n, "status": "timeout",
                  "reps": 0, "wedged": True, "not_run": True, "wedged_at_n": hit[0], "timeout_s": hit[1],
                  "stalls": {"count": 0, "max_ms": 0.0, "total_ms": 0.0},
                  "error": "not run: wedged Natron for over %.0f s at n=%d" % (hit[1], hit[0])}
        emit(record)
        report("RESULT " + json.dumps(record, sort_keys=True))
        check(False, "%s %s n=%d: timeout (%s)" % (name, topology, n, record["error"]))
        return True

    def bench_graph(topology, n, workdir, previous_path):
        plan = op_plan(topology, n)
        if not plan:
            report("NOTE %s-%d: every op is already recorded" % (topology, n))
            return previous_path
        gt.section("%s-%d" % (topology, n))
        plan = [p for p in plan if not record_not_run(p[0], p[2], topology, n)]
        if not plan:
            return previous_path
        if previous_path is not None:
            clean_reset(previous_path)
            yield ("sleep", SETTLE_MS)
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
        front_root_graph()
        path = os.path.join(workdir, "%s-%d.ntpz" % (topology, n))
        gt.require(app.saveProjectAs(path), "%s n=%d: saved to %s" % (topology, n, path))
        ctx = Ctx(topology, n, summary, path)
        activate()
        fit()
        yield ("sleep", SETTLE_MS)
        edits = [p for p in plan if p[2] == "edit"]
        first = [p for p in plan if p[2] == "interaction" and p[0] != "load"]
        last = [p for p in plan if p not in edits and p not in first]
        for name, make, kind in first:
            for cmd in run_op(name, make, ctx, kind):
                yield cmd
        before = root_names()
        for name, make, kind in edits:
            for cmd in run_op(name, make, ctx, kind):
                yield cmd
        if edits:
            after = root_names()
            check(after == before, "%s n=%d: the edit ops leave the root graph as they found it (%d -> %d nodes)" %
                  (topology, n, len(before), len(after)))
        for name, make, kind in last:
            for cmd in run_op(name, make, ctx, kind):
                yield cmd
        return path

    def steps():
        ok = yield ("until", lambda: root_graph() is not None and root_graph().isVisible(), STARTUP_TIMEOUT_S)
        gt.require(ok, "wait expired: the node graph was not visible within %.0f s of Natron starting" % STARTUP_TIMEOUT_S)
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
