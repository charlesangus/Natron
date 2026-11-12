"""A GUI test that Tab still opens the node graph's tab menu after every way of leaving global mode.
It is driven by Tests/gui/run-gui-test.sh, not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m399-gui/tab-after-global-mode GUI_TEST_TIMEOUT=600 \
        Tests/gui/run-gui-test.sh Tests/gui/m399_tab_after_global_mode.py <AppImage or build/fast/App/Natron>'

The pointer, the buttons and the keys are driven through the X server's XTEST extension, so Qt
sees the same events a user makes: pointer grabs, enter and leave crossings, keyboard focus and
the shortcut-override step all happen for real. Only the probe key is sent with sendEvent, since
an app-wide event filter sees those too.

For each target (Blur size, a lone value knob; Grade white, the colour trackball; Transform
rotate, a transform group) and each exit (Esc, Enter, a right-click, the application losing
focus), the case hovers the knob in its settings panel, engages with Ctrl+E, strokes over the
node graph, exits, and then brings the pointer to the graph either by leaving it for the panel
and coming back (a fresh Enter) or by a move that stays inside it (no Enter). It then checks
that no override cursor is left, that a probe key reaches the graph, and that a real Tab opens
the tab menu, which is closed with Esc before the next case. A baseline without global mode runs
first, so a harness problem is not mistaken for the bug.

Widget wrappers are looked up again after every yield: a wrapper kept across one can outlive its
C++ object, and children found through a collected parent's wrapper die with it.
"""
import ctypes
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 540
ACTIVATE_TIMEOUT_S = 10.0
MENU_TIMEOUT_S = 2.0
DEACTIVATE_TIMEOUT_S = 3.0
PANEL_MS = 600
POINTER_MS = 250
KEY_MS = 120
STROKE_STEP_MS = 15

TARGETS = (
    ("value", "Blur", "size"),
    ("colour", "Grade", "white"),
    ("group", "Transform", "rotate"),
)
EXITS = ("esc", "enter", "right-click", "deactivate")
ENTRIES = ("fresh-enter", "no-enter")

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QObject, QPoint, Qt
    from PySide6.QtGui import QCursor, QGuiApplication, QKeyEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QMainWindow, QScrollArea, QWidget

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    class XInput(object):
        """Pointer, button and key events injected through XTEST, as if from real devices."""

        def __init__(self):
            self.error = None
            self.dpy = None
            try:
                self.x11 = ctypes.CDLL("libX11.so.6")
                self.xtst = ctypes.CDLL("libXtst.so.6")
            except OSError as e:
                self.error = "no libX11/libXtst (%s)" % e
                return
            x11, xtst = self.x11, self.xtst
            x11.XOpenDisplay.restype = ctypes.c_void_p
            x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
            x11.XFlush.argtypes = [ctypes.c_void_p]
            x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
            x11.XStringToKeysym.restype = ctypes.c_ulong
            x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
            x11.XKeysymToKeycode.restype = ctypes.c_ubyte
            x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
            xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
            xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
            xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
            self.dpy = x11.XOpenDisplay(None)
            if not self.dpy:
                self.error = "cannot open the display"

        def _flush(self):
            self.x11.XSync(self.dpy, 0)

        def move(self, p):
            ratio = QGuiApplication.primaryScreen().devicePixelRatio()
            self.xtst.XTestFakeMotionEvent(self.dpy, -1, int(round(p.x() * ratio)), int(round(p.y() * ratio)), 0)
            self._flush()

        def button(self, number, down):
            self.xtst.XTestFakeButtonEvent(self.dpy, number, 1 if down else 0, 0)
            self._flush()

        def keycode(self, name):
            return self.x11.XKeysymToKeycode(self.dpy, self.x11.XStringToKeysym(name.encode("ascii")))

        def key(self, name, down):
            self.xtst.XTestFakeKeyEvent(self.dpy, self.keycode(name), 1 if down else 0, 0)
            self._flush()

        def focus(self, window_id):
            """Gives X keyboard focus to window_id. Needed after unfocus(): Qt's own request carries
            its last event time, which is older than the focus change and so is ignored."""
            self.x11.XSetInputFocus(self.dpy, window_id, 1, 0)
            self._flush()

        def unfocus(self):
            """Gives X keyboard focus to no window, which deactivates the application."""
            self.x11.XSetInputFocus(self.dpy, 0, 0, 0)
            self._flush()

    class GraphRecorder(QObject):
        """Records what reaches the root graph's view; eats the probe key so it triggers nothing."""

        def __init__(self):
            QObject.__init__(self)
            self.probe = False
            self.tab = False

        def eventFilter(self, obj, event):
            t = event.type()
            if t in (QEvent.KeyPress, QEvent.KeyRelease):
                key = event.key()
                if key == Qt.Key_F35:
                    if t == QEvent.KeyPress:
                        self.probe = True
                    return True
                if key == Qt.Key_Tab and t == QEvent.KeyPress:
                    self.tab = True
            return False

    xin = XInput()
    recorder = GraphRecorder()

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        for w in gt.widgets_of("NodeGraph"):
            try:
                if w.isVisible() and w.objectName() == "":
                    return w
            except RuntimeError:
                continue
        return None

    def graph_point(dx=0, dy=0):
        view = root_graph()
        c = view.viewport().mapToGlobal(view.viewport().rect().center())
        return QPoint(c.x() + dx, c.y() + dy)

    def pointer_over_graph():
        view = root_graph()
        vp = view.viewport()
        return vp.rect().contains(vp.mapFromGlobal(QCursor.pos()))

    def popup():
        for w in gt.widgets_of("TabTabTabWidget"):
            try:
                if w.isVisible():
                    return w
            except RuntimeError:
                continue
        return None

    def cursor_name():
        c = QApplication.overrideCursor()
        if c is None:
            return "none"
        shape = c.shape()
        if shape == Qt.CrossCursor:
            return "cross"
        if shape == Qt.BlankCursor:
            return "blank"
        return str(shape)

    def engaged():
        return QApplication.overrideCursor() is not None

    def shortcut_action():
        for a in main_window().actions():
            if a.shortcut().toString() == "Ctrl+E" or "Hovered or Last-Touched" in a.text():
                return a
        return None

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            try:
                if not panel.isVisible():
                    continue
                for w in panel.findChildren(QWidget):
                    if gt.class_name(w) == "LineEdit" and w.text() == label:
                        return panel
            except RuntimeError:
                continue
        return None

    def raise_panel(node):
        app.clearSelection()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(root_graph(), "showSelectedNodeSettingsPanel")
        QApplication.processEvents()

    def knob_point(node, name):
        """The global centre of a visible, non-label widget of the knob, scrolled into view.

        KnobGui puts the knob's script name in bold at the start of its tooltip."""
        panel = panel_of(node)
        if panel is None:
            return None
        for w in panel.findChildren(QWidget):
            try:
                if isinstance(w, QLabel) or w.width() < 4 or w.height() < 4:
                    continue
                if ("<b>%s</b>" % name) not in (w.toolTip() or "")[:200]:
                    continue
                a = w.parentWidget()
                while a is not None:
                    if isinstance(a, QScrollArea):
                        a.ensureWidgetVisible(w)
                        break
                    a = a.parentWidget()
                QApplication.processEvents()
                if w.isVisible() and not w.visibleRegion().isEmpty():
                    return w.mapToGlobal(w.rect().center())
            except RuntimeError:
                continue
        return None

    def panel_point(node):
        panel = panel_of(node)
        return panel.mapToGlobal(QPoint(panel.width() // 2, 12)) if panel is not None else None

    def val(node, name):
        p = node.getParam(name)
        return tuple(round(p.getValue(d), 4) for d in range(p.getNumDimensions()))

    def activate():
        win = main_window()
        if win is None:
            return False
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: main_window() is not None and main_window().isActiveWindow(), 1.0)
        if not ok:
            xin.focus(int(main_window().winId()))
            ok = yield ("until", lambda: main_window() is not None and main_window().isActiveWindow(),
                        ACTIVATE_TIMEOUT_S)
        return ok

    def move(p):
        xin.move(p)
        yield ("sleep", POINTER_MS)

    def tap(name, mods=()):
        for m in mods:
            xin.key(m, True)
        xin.key(name, True)
        xin.key(name, False)
        for m in reversed(mods):
            xin.key(m, False)
        yield ("sleep", KEY_MS)

    def stroke():
        start = graph_point()
        yield from move(start)
        xin.button(1, True)
        yield ("sleep", 60)
        for k in range(1, 11):
            xin.move(QPoint(start.x() + 4 * k, start.y() - 2 * k))
            yield ("sleep", STROKE_STEP_MS)
        xin.button(1, False)
        yield ("sleep", POINTER_MS)

    def engage(tag, node, knob):
        raise_panel(node)
        yield ("sleep", PANEL_MS)
        p = knob_point(node, knob)
        if not check(p is not None, "%s: %s.%s is on screen in its panel" % (tag, node.getLabel(), knob)):
            return False
        yield from move(p)
        ok = yield from activate()
        if not ok:
            report("NOTE %s: the main window did not report active before Ctrl+E" % tag)
        yield from tap("e", ("Control_L",))
        if not engaged():
            a = shortcut_action()
            if a is not None:
                a.trigger()
                QApplication.processEvents()
                report("NOTE %s: the real Ctrl+E did not engage; the action was triggered instead" % tag)
        return check(engaged(), "%s: Ctrl+E over %s.%s engages global mode" % (tag, node.getLabel(), knob))

    def leave_by(tag, how):
        if how == "esc":
            yield from tap("Escape")
        elif how == "enter":
            yield from tap("Return")
        elif how == "right-click":
            xin.button(3, True)
            yield ("sleep", 60)
            xin.button(3, False)
            yield ("sleep", POINTER_MS)
        elif how == "deactivate":
            xin.unfocus()
            ok = yield ("until", lambda: QGuiApplication.applicationState() != Qt.ApplicationActive, DEACTIVATE_TIMEOUT_S)
            if not ok:
                QApplication.sendEvent(QApplication.instance(), QEvent(QEvent.ApplicationDeactivate))
                report("NOTE %s: X focus loss did not deactivate the application; ApplicationDeactivate was sent" % tag)
            yield ("sleep", POINTER_MS)
            back = yield from activate()
            check(back, "%s: the main window is active again after the deactivation" % tag)
        report("NOTE %s: after the exit the override cursor is %s" % (tag, cursor_name()))

    def come_back(how, node):
        if how == "fresh-enter":
            away = panel_point(node)
            if away is not None:
                yield from move(away)
            yield from move(graph_point(20, 12))
        else:
            yield from move(graph_point(20, 12))

    def tab_checks(tag):
        """The checks after the pointer is back on the graph; returns the names of those that failed."""
        missed = []
        if not check(QApplication.overrideCursor() is None, "%s: no override cursor is left (%s)" % (tag, cursor_name())):
            missed.append("cursor")
        recorder.probe = False
        graph = root_graph()
        QApplication.sendEvent(graph, QKeyEvent(QEvent.KeyPress, Qt.Key_F35, Qt.NoModifier))
        QApplication.sendEvent(graph, QKeyEvent(QEvent.KeyRelease, Qt.Key_F35, Qt.NoModifier))
        QApplication.processEvents()
        if not check(recorder.probe, "%s: a probe key reaches the graph (no app-wide filter eats it)" % tag):
            missed.append("filter")

        focus = QApplication.focusWidget()
        focus_name = gt.class_name(focus) if focus is not None else "none"
        graph_focused = graph.hasFocus()
        over = pointer_over_graph()
        recorder.tab = False
        yield from tap("Tab")
        opened = yield ("until", lambda: popup() is not None, MENU_TIMEOUT_S)
        report("NOTE %s: at Tab the pointer is %s the graph, focus is on %s (graph focused: %s), "
               "the Tab press %s the graph" % (tag, "over" if over else "off", focus_name, graph_focused,
                                                "reached" if recorder.tab else "did not reach"))
        if not check(opened, "%s: Tab opens the tab menu" % tag):
            missed.append("tab")
        p = popup()
        if p is not None:
            QTest.keyClick(p, Qt.Key_Escape)
            yield ("until", lambda: popup() is None, MENU_TIMEOUT_S)
        return missed

    def recover(tag):
        """Leaves global mode if a failed exit left it on, so the next case starts clean."""
        if engaged():
            yield from tap("Escape")
        if engaged():
            win = main_window()
            QApplication.sendEvent(win, QKeyEvent(QEvent.KeyPress, Qt.Key_Escape, Qt.NoModifier))
            QApplication.sendEvent(win, QKeyEvent(QEvent.KeyRelease, Qt.Key_Escape, Qt.NoModifier))
            yield ("sleep", KEY_MS)
        if engaged():
            report("NOTE %s: global mode could not be left; later cases start engaged" % tag)

    def steps():
        gt.section("setup")
        require(xin.error is None, "XTEST input is available", xin.error or "")
        ok = yield ("until", lambda: root_graph() is not None and main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the root node graph is up")
        ok = yield from activate()
        require(ok, "the main window is active")
        gt.close_all_panels()
        NatronEngine.natron.getSettings().getParam("colorTrackballFeedback").set(0)
        nodes = {}
        for i, (kind, plugin_id, _knob) in enumerate(TARGETS):
            nodes[kind] = create(plugin_id, -400 + 200 * i, -300)
        yield ("until", lambda: all(panel_of(n) is not None for n in nodes.values()), 20.0)
        yield ("sleep", PANEL_MS)
        root_graph().installEventFilter(recorder)

        p = graph_point(5, 5)
        yield from move(p)
        moved = QCursor.pos()
        require(abs(moved.x() - p.x()) <= 1 and abs(moved.y() - p.y()) <= 1,
                "an XTEST move puts the pointer where it was sent (%d,%d vs %d,%d)" % (moved.x(), moved.y(), p.x(), p.y()))

        gt.section("baseline")
        for entry in ENTRIES:
            tag = "baseline %s" % entry
            yield from come_back(entry, nodes["value"])
            missed = yield from tab_checks(tag)
            require(not missed, "%s: the harness passes every check without global mode" % tag)

        failing = []
        for kind, _plugin_id, knob in TARGETS:
            node = nodes[kind]
            for how in EXITS:
                for entry in ENTRIES:
                    tag = "%s %s %s" % (kind, how, entry)
                    gt.section(tag)
                    on = yield from engage(tag, node, knob)
                    if not on:
                        failing.append(tag + " (did not engage)")
                        continue
                    before = val(node, knob)
                    yield from stroke()
                    after = val(node, knob)
                    check(after != before, "%s: the stroke edits %s (%r -> %r)" % (tag, knob, before, after))
                    yield from leave_by(tag, how)
                    yield from come_back(entry, node)
                    missed = yield from tab_checks(tag)
                    if missed:
                        failing.append("%s (%s)" % (tag, "/".join(missed)))
                    yield from recover(tag)

        gt.section("summary")
        report("NOTE failing paths: %s" % (", ".join(failing) if failing else "none"))
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
