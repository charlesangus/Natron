"""A GUI test that a properties panel is never on screen half built.
It is driven by Tests/gui/run-gui-test.sh, not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m431-gui/panel-appears-whole \
        Tests/gui/run-gui-test.sh Tests/gui/m431_panel_appears_whole.py <AppImage or build/release/App/Natron>'

Blur, Merge and Roto are created with their create-node shortcuts, and the panel of an existing
Merge is opened by double-clicking the node. Keys and clicks go through the X server's XTEST
extension, so they reach Natron as a user's do. A zero-delay timer chain samples every pass of the
event loop: on each pass a NodeSettingsPanel is visible, it records how many knob rows of the
panel's current page are visible. Once the panel has settled, every sample of the node's panel
must equal its final count, so a panel that showed some rows and gained the rest later fails.
Screenshots of the Merge and Roto panels are saved as panel_Merge.png and panel_Roto.png.

Two more cases create a Blur and undo at once, one before the node has painted and one right
after: Natron must not crash, the node must be gone and no panel may be left open.

Widget wrappers are looked up again on every pass and never kept across a yield: a wrapper kept
across one can outlive its C++ object.
"""
import ctypes
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
ACTIVATE_TIMEOUT_S = 10.0
PANEL_TIMEOUT_S = 10.0
SETTLE_MS = 1000
POINTER_MS = 250
KEY_MS = 120
DOUBLE_CLICK_GAP_MS = 60

SHORTCUT_PLUGINS = ("Blur", "Merge", "Roto")
SHOT_PLUGINS = ("Merge", "Roto")
DOUBLE_CLICK_PLUGIN = "Merge"
UNDO_PLUGIN = "Blur"

PROP_ID = "natronPluginID"
PROP_SHORTCUT = "natronShortcut"
ROW_CLASS = "RightClickableWidget"
MODIFIER_KEYSYMS = {"shift": "Shift_L", "ctrl": "Control_L", "alt": "Alt_L", "meta": "Super_L"}

try:
    import NatronGui
    import shiboken6
    from PySide6.QtCore import QObject, QPoint, QPointF, QTimer
    from PySide6.QtGui import QCursor, QGuiApplication
    from PySide6.QtWidgets import QApplication, QMainWindow, QWidget

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

        def chord(self, keysyms):
            """Presses keysyms in order and releases them in reverse, without waiting."""
            for k in keysyms:
                self.key(k, True)
            for k in reversed(keysyms):
                self.key(k, False)

        def focus(self, window_id):
            self.x11.XSetInputFocus(self.dpy, window_id, 1, 0)
            self._flush()

    class RowSampler(QObject):
        """Samples, on every event-loop pass, the visible knob rows of every visible properties panel.

        samples maps a panel's C++ address to the row count of each pass it was visible on."""

        def __init__(self):
            QObject.__init__(self)
            self.samples = {}
            self.running = False
            self.passes = 0

        def start(self):
            self.samples = {}
            self.passes = 0
            if not self.running:
                self.running = True
                QTimer.singleShot(0, self.tick)

        def stop(self):
            self.running = False

        def tick(self):
            if not self.running:
                return
            self.passes += 1
            for panel in gt.widgets_of("NodeSettingsPanel"):
                try:
                    if panel.isVisible():
                        self.samples.setdefault(ptr(panel), []).append(visible_rows(panel))
                except RuntimeError:
                    continue
            panel = None
            QTimer.singleShot(0, self.tick)

    xin = XInput()
    sampler = RowSampler()

    def ptr(obj):
        return shiboken6.getCppPointer(obj)[0]

    def visible_rows(panel):
        """The knob rows of the panel's current page that are on screen; rows of other pages and of
        closed groups are hidden with their ancestors."""
        count = 0
        for w in panel.findChildren(QWidget):
            try:
                if gt.class_name(w) == ROW_CLASS and w.isVisible():
                    count += 1
            except RuntimeError:
                continue
        return count

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

    def visible_panels():
        found = []
        for panel in gt.widgets_of("NodeSettingsPanel"):
            try:
                if panel.isVisible():
                    found.append(panel)
            except RuntimeError:
                continue
        return found

    def panel_of(node):
        label = node.getLabel()
        for panel in visible_panels():
            try:
                for w in panel.findChildren(QWidget):
                    if gt.class_name(w) == "LineEdit" and w.text() == label:
                        return panel
            except RuntimeError:
                continue
        return None

    def root_names():
        return set(n.getScriptName() for n in app.getChildren())

    def new_node(before):
        added = sorted(root_names() - before)
        return app.getNode(added[0]) if len(added) == 1 else None

    def plugin_action(plugin_id, menu=None):
        menu = menu if menu is not None else NatronGui.natron.nodesMenu()
        for a in menu.actions():
            sub = gt.menu_of(a)
            if sub is not None:
                found = plugin_action(plugin_id, sub)
                if found is not None:
                    return found
            elif a.property(PROP_ID) == plugin_id:
                return a
        return None

    def shortcut_keysyms(plugin_id):
        """The XTEST keysyms of the plug-in's create-node shortcut, modifiers first, or None."""
        a = plugin_action(plugin_id)
        text = str(a.property(PROP_SHORTCUT) or "") if a is not None else ""
        if not text or "," in text:
            return None, text
        parts = text.split("+")
        keysyms = []
        for mod in parts[:-1]:
            sym = MODIFIER_KEYSYMS.get(mod.strip().lower())
            if sym is None:
                return None, text
            keysyms.append(sym)
        key = parts[-1].strip()
        keysyms.append(key.lower() if len(key) == 1 else key)
        return keysyms, text

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

    def ready_for_keys():
        """Leaves the pointer over the graph and the window active, which the create-node keys need."""
        yield from move(graph_point(-60, 40))
        ok = yield from activate()
        if not ok:
            report("NOTE the main window did not report active before the key")
        view = root_graph()
        view.viewport().setFocus()
        yield ("sleep", KEY_MS)

    def quiet():
        gt.close_all_panels()
        app.clearSelection()
        yield ("sleep", SETTLE_MS)

    def check_whole(tag, node):
        """Waits for node's panel to settle, then checks that every pass it was visible on showed the
        rows it ends with."""
        ok = yield ("until", lambda: panel_of(node) is not None, PANEL_TIMEOUT_S)
        if not check(ok, "%s: the panel of %s opens" % (tag, node.getLabel())):
            sampler.stop()
            return False
        yield ("sleep", SETTLE_MS)
        sampler.stop()
        panel = panel_of(node)
        if not check(panel is not None, "%s: the panel of %s is still open once settled" % (tag, node.getLabel())):
            return False
        final = visible_rows(panel)
        seen = sampler.samples.get(ptr(panel), [])
        panel = None
        report("NOTE %s: %d visible passes over %d sampled, final %d rows, first counts %r" % (
            tag, len(seen), sampler.passes, final, seen[:8]))
        check(final > 0, "%s: the settled panel shows knob rows" % tag)
        check(len(seen) > 0, "%s: the sampler saw the panel while it was visible" % tag)
        partial = sorted(set(n for n in seen if n != final))
        check(not partial, "%s: every visible pass showed all %d rows (also saw %r)" % (tag, final, partial))
        return True

    def save_panel(name, node):
        panel = panel_of(node)
        if panel is None:
            report("NOTE no panel to save for " + name)
            return
        path = os.path.join(gt.OUT, name)
        if panel.grab().save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def node_point(node):
        """The node's centre on screen, after scrolling the graph to it."""
        graph = root_graph()
        x, y = node.getPosition()
        w, h = node.getSize()
        centre = QPointF(x + w / 2.0, y + h / 2.0)
        graph.centerOn(centre)
        QApplication.processEvents()
        p = graph.mapFromScene(centre)
        return graph.viewport().mapToGlobal(p)

    def steps():
        gt.section("setup")
        require(xin.error is None, "XTEST input is available", xin.error or "")
        ok = yield ("until", lambda: root_graph() is not None and main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the root node graph is up")
        ok = yield from activate()
        require(ok, "the main window is active")
        p = graph_point(5, 5)
        yield from move(p)
        moved = QCursor.pos()
        require(abs(moved.x() - p.x()) <= 1 and abs(moved.y() - p.y()) <= 1,
                "an XTEST move puts the pointer where it was sent (%d,%d vs %d,%d)" % (moved.x(), moved.y(), p.x(), p.y()))
        keys = {}
        for pid in SHORTCUT_PLUGINS + (UNDO_PLUGIN,):
            syms, text = shortcut_keysyms(pid)
            require(syms is not None, "%s has a single-key create-node shortcut (got %r)" % (pid, text),
                    gt.plugin_problem(pid))
            keys[pid] = syms
        undo_keys = ["Control_L", "z"]
        yield from quiet()

        for pid in SHORTCUT_PLUGINS:
            tag = "shortcut %s" % pid
            gt.section(tag)
            yield from quiet()
            yield from ready_for_keys()
            before = root_names()
            sampler.start()
            xin.chord(keys[pid])
            ok = yield ("until", lambda: new_node(before) is not None, PANEL_TIMEOUT_S)
            if not check(ok, "%s: the shortcut creates exactly one node" % tag):
                sampler.stop()
                continue
            node = new_node(before)
            check(node.getPluginID() == pid, "%s: the new node is a %s (got %s)" % (tag, pid, node.getPluginID()))
            whole = yield from check_whole(tag, node)
            if whole and pid in SHOT_PLUGINS:
                save_panel("panel_%s.png" % pid, node)

        tag = "double-click %s" % DOUBLE_CLICK_PLUGIN
        gt.section(tag)
        node = app.createNode(DOUBLE_CLICK_PLUGIN)
        require(node is not None, "create a %s from Python" % DOUBLE_CLICK_PLUGIN, gt.plugin_problem(DOUBLE_CLICK_PLUGIN))
        node.setPosition(300, -250)
        yield from quiet()
        check(panel_of(node) is None, "%s: the node's panel is closed before the double-click" % tag)
        target = node_point(node)
        yield from move(target)
        ok = yield from activate()
        if not ok:
            report("NOTE %s: the main window did not report active before the double-click" % tag)
        sampler.start()
        xin.button(1, True)
        xin.button(1, False)
        yield ("sleep", DOUBLE_CLICK_GAP_MS)
        xin.button(1, True)
        xin.button(1, False)
        yield from check_whole(tag, node)

        for when in ("before-paint", "after-paint"):
            tag = "undo %s %s" % (UNDO_PLUGIN, when)
            gt.section(tag)
            yield from quiet()
            yield from ready_for_keys()
            before = root_names()
            xin.chord(keys[UNDO_PLUGIN])
            if when == "after-paint":
                ok = yield ("until", lambda: new_node(before) is not None, PANEL_TIMEOUT_S)
                check(ok, "%s: the shortcut creates the node" % tag)
            xin.chord(undo_keys)
            ok = yield ("until", lambda: root_names() == before, PANEL_TIMEOUT_S)
            check(ok, "%s: Ctrl+Z straight after the shortcut leaves the graph as it was (added %r)" % (
                tag, sorted(root_names() - before)))
            yield ("sleep", SETTLE_MS)
            check(root_names() == before, "%s: the node stays gone once settled" % tag)
            check(not visible_panels(), "%s: no panel is left open (%d open)" % (tag, len(visible_panels())))

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
