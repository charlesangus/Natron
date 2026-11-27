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

A last case checks the focus order of rows built after the panel: a parameter hidden when the panel
is built is shown, and Tab from the last focusable widget of the row above it must land in it;
hiding it again while it has the focus must leave the focus in the same panel without scrolling the
Properties bin; and Shift+Tab from the first widget of a page built when first shown must stay in
the panel. A second panel is opened after the first, so a misplaced widget would lead into it.

A lazy-page case opens a panel and picks a one-dimensional numeric parameter on a page not shown
yet whose row is not built: Python must read it and set a new value before the page is shown, and
showing the page must build the row with that value in its field.

A hide-unmodified case turns on the panel's hide-unmodified button, then shows a page that was not
shown yet: the unmodified rows of that page (any type with a label) must stay hidden, as they would on
a page built with the panel, and must show once the button is turned off again.

An unfold case opens a Grade panel, whose multiply parameter starts folded to one field: clicking
its dimension switch must show a field per channel, holding the parameter's values, with Tab going
from the second field to the third, and typing into the third field must set that channel alone.

Widget wrappers are looked up again on every pass and never kept across a yield: a wrapper kept
across one can outlive its C++ object.
"""
import ctypes
import os
import re
import sys
import time
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
SCROLL_TOLERANCE_PX = 100
FOCUS_CHAIN_STEPS = 5000

SHORTCUT_PLUGINS = ("Blur", "Merge", "Roto")
SHOT_PLUGINS = ("Merge", "Roto")
DOUBLE_CLICK_PLUGIN = "Merge"
UNDO_PLUGIN = "Blur"
# Tried first for a parameter hidden at panel build; the other plug-ins follow, up to LAZY_SCAN_LIMIT.
LAZY_PLUGINS = ("Merge", "Transform", "Grade", "ColorCorrect", "Blur", "Shuffle", "Constant", "Text")
LAZY_SKIP = ("read", "write", "viewer", "group", "backdrop", "dot", "input", "output", "precomp",
             "tracker", "roto", "cache", "disk")
LAZY_SCAN_LIMIT = 60
LAZY_PANEL_LIMIT = 12
SECOND_PANEL_PLUGIN = "Blur"
UNFOLD_PLUGIN = "Grade"
UNFOLD_PARAM = "multiply"
UNFOLD_VALUE = 0.25
# Tried in turn for a page other than the one a new panel shows, holding unmodified rows.
UNMODIFIED_PLUGINS = ("Merge", "Transform", "Grade", "Blur", "ColorCorrect")
HIDE_UNMODIFIED_TIP = "without modifications"
LABEL_CLASS = "ClickableLabel"

PROP_ID = "natronPluginID"
PROP_SHORTCUT = "natronShortcut"
ROW_CLASS = "RightClickableWidget"
MODIFIER_KEYSYMS = {"shift": "Shift_L", "ctrl": "Control_L", "alt": "Alt_L", "meta": "Super_L"}

try:
    import NatronEngine
    import NatronGui
    import shiboken6
    from PySide6.QtCore import QMetaObject, QObject, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QCursor, QGuiApplication
    from PySide6.QtWidgets import (QAbstractButton, QApplication, QLabel, QLineEdit, QMainWindow, QScrollArea,
                                   QTabWidget, QWidget)

    from PySide6.QtTest import QTest
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

    def hidden_row_param(node):
        """The script name of a number parameter of node's first page that is hidden, sits on its own
        line below a shown one, and has a shown number parameter somewhere below it; or None."""
        params = node.getParams()
        first_page = None
        for p in params:
            if isinstance(p, NatronEngine.PageParam):
                first_page = p.getScriptName()
                break
        if first_page is None:
            return None
        siblings = []
        for p in params:
            parent = p.getParent()
            if not isinstance(p, NatronEngine.PageParam) and parent is not None and parent.getScriptName() == first_page:
                siblings.append(p)
        numbers = (NatronEngine.DoubleParam, NatronEngine.IntParam)
        for i in range(1, len(siblings) - 1):
            p, prev = siblings[i], siblings[i - 1]
            if p.getIsVisible() or not isinstance(p, numbers):
                continue
            if not prev.getIsVisible() or isinstance(prev, NatronEngine.GroupParam):
                continue
            if not (p.getAddNewLine() and prev.getAddNewLine()):
                continue
            if any(s.getIsVisible() and isinstance(s, numbers) for s in siblings[i + 1:]):
                return p.getScriptName()
        return None

    def find_hidden_row_node():
        """A new node with a parameter hidden at panel build, and that parameter's script name."""
        ids = list(LAZY_PLUGINS)
        for pid in sorted(str(i) for i in NatronEngine.natron.getPluginIDs()):
            if pid not in ids and not any(k in pid.lower() for k in LAZY_SKIP):
                ids.append(pid)
        tried = []
        for pid in ids[:LAZY_SCAN_LIMIT]:
            node = app.createNode(pid)
            if node is None:
                continue
            tried.append(pid)
            name = hidden_row_param(node)
            if name is not None:
                return node, name, tried
            node.destroy()
        return None, None, tried

    def open_panel(node):
        app.clearSelection()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(root_graph(), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, PANEL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        return ok

    def label_text(w):
        return re.sub(r"<[^>]+>", "", w.text()).strip()

    def labels_named(panel, text):
        return [w for w in panel.findChildren(QLabel) if gt.class_name(w) == LABEL_CLASS and label_text(w) == text]

    def takes_tab(w):
        return (bool(w.focusPolicy().value & Qt.FocusPolicy.TabFocus.value)
                and w.isVisible() and w.isEnabled())

    def tab_neighbour(w, forward):
        """The widget Tab (or Shift+Tab) would move the focus to from w."""
        n = w.nextInFocusChain() if forward else w.previousInFocusChain()
        for _ in range(FOCUS_CHAIN_STEPS):
            if n is None or n is w:
                return None
            if takes_tab(n):
                return n
            n = n.nextInFocusChain() if forward else n.previousInFocusChain()
        return None

    def grid_row_takers(layout, row):
        found, seen = [], set()
        for col in (0, 1):
            item = layout.itemAtPosition(row, col)
            w = item.widget() if item is not None else None
            if w is None:
                continue
            for c in [w] + w.findChildren(QWidget):
                if takes_tab(c) and ptr(c) not in seen:
                    seen.add(ptr(c))
                    found.append(c)
        return found

    def row_of_label(panel, label_ptr):
        """The grid of the row whose label is at label_ptr, the row index, and its focusable widgets."""
        if panel is None:
            return None, -1, []
        for w in panel.findChildren(QLabel):
            if ptr(w) != label_ptr:
                continue
            container = w.parentWidget()
            owner = container.parentWidget() if container is not None else None
            layout = owner.layout() if owner is not None else None
            if layout is None or not hasattr(layout, "itemAtPosition"):
                return None, -1, []
            row = layout.getItemPosition(layout.indexOf(container))[0]
            return layout, row, grid_row_takers(layout, row)
        return None, -1, []

    def last_in_tab_order(widgets):
        ptrs = set(ptr(w) for w in widgets)
        for w in widgets:
            n = tab_neighbour(w, True)
            if n is None or ptr(n) not in ptrs:
                return w
        return None

    def first_in_tab_order(widgets):
        ptrs = set(ptr(w) for w in widgets)
        for w in widgets:
            n = tab_neighbour(w, False)
            if n is None or ptr(n) not in ptrs:
                return w
        return None

    def bin_scroll(panel):
        w = panel.parentWidget() if panel is not None else None
        while w is not None:
            if isinstance(w, QScrollArea):
                return w.verticalScrollBar().value()
            w = w.parentWidget()
        return None

    def panel_tabs(panel):
        """The panel's page tab widget, not the tab widget of a group shown as tabs."""
        for t in panel.findChildren(QTabWidget):
            w, in_group = t.parentWidget(), False
            while w is not None and ptr(w) != ptr(panel):
                if gt.class_name(w) == "TabGroup":
                    in_group = True
                    break
                w = w.parentWidget()
            if not in_group:
                return t
        return None

    def focus_in(node):
        fw = QApplication.focusWidget()
        panel = panel_of(node)
        inside = fw is not None and panel is not None and panel.isAncestorOf(fw)
        name = "%s(%s)" % (gt.class_name(fw), fw.objectName()) if fw is not None else "none"
        return inside, name

    def lazy_focus_steps():
        tag = "lazy focus"
        gt.section(tag)
        yield from quiet()
        node, pname, tried = find_hidden_row_node()
        require(node is not None, "%s: a plug-in has a number parameter hidden at panel build" % tag,
                "tried %r" % tried)
        report("NOTE %s: %s.%s is hidden at panel build" % (tag, node.getPluginID(), pname))
        other = app.createNode(SECOND_PANEL_PLUGIN)
        require(other is not None, "%s: create a %s" % (tag, SECOND_PANEL_PLUGIN), gt.plugin_problem(SECOND_PANEL_PLUGIN))
        other.setPosition(400, 200)
        ok = yield from open_panel(node)
        require(ok, "%s: the panel of %s opens" % (tag, node.getLabel()))
        ok = yield from open_panel(other)
        require(ok and panel_of(node) is not None, "%s: the panels of %s and %s are open together" % (
            tag, node.getLabel(), other.getLabel()))

        param = node.getParam(pname)
        text = param.getLabel()
        panel = panel_of(node)
        before = set(ptr(w) for w in labels_named(panel, text))
        panel = None
        param.setVisible(True)
        yield ("sleep", SETTLE_MS)
        panel = panel_of(node)
        new = [ptr(w) for w in labels_named(panel, text) if ptr(w) not in before] if panel is not None else []
        panel = None
        if check(len(new) == 1, "%s: showing %s builds its row then (%d new labels)" % (tag, pname, len(new))):
            ok = yield from activate()
            if not ok:
                report("NOTE %s: the main window did not report active before Tab" % tag)
            panel = panel_of(node)
            layout, row, takers = row_of_label(panel, new[0])
            new_ptrs = set(ptr(w) for w in takers)
            prev = []
            for r in range(row - 1, -1, -1):
                prev = grid_row_takers(layout, r) if layout is not None else []
                if prev:
                    break
            last = last_in_tab_order(prev)
            layout = None
            ready = check(bool(takers) and last is not None, "%s: the shown row and a row above it take focus "
                          "(%d and %d widgets)" % (tag, len(takers), len(prev)))
            if ready:
                last.setFocus(Qt.TabFocusReason)
            last = None
            prev = takers = []
            panel = None
            if ready:
                yield ("sleep", KEY_MS)
                xin.chord(["Tab"])
                yield ("sleep", KEY_MS * 2)
                fw = QApplication.focusWidget()
                landed = fw is not None and ptr(fw) in new_ptrs
                inside, name = focus_in(node)
                check(landed, "%s: Tab from the row above lands in the shown row (went to %s, in panel %r)" % (
                    tag, name, inside))
                fw = None

                panel = panel_of(node)
                _, _, takers = row_of_label(panel, new[0])
                first = first_in_tab_order(takers)
                if first is not None:
                    first.setFocus(Qt.TabFocusReason)
                first = None
                takers = []
                scroll_before = bin_scroll(panel)
                panel = None
                yield ("sleep", KEY_MS)
                param.setVisible(False)
                yield ("sleep", SETTLE_MS)
                inside, name = focus_in(node)
                check(inside, "%s: hiding the focused row leaves the focus in its panel (went to %s)" % (tag, name))
                scroll_after = bin_scroll(panel_of(node))
                moved = abs(scroll_after - scroll_before) if None not in (scroll_before, scroll_after) else 0
                check(moved <= SCROLL_TOLERANCE_PX, "%s: hiding the focused row does not scroll the Properties "
                      "bin (%r -> %r)" % (tag, scroll_before, scroll_after))

        panel = panel_of(node)
        tabs = panel_tabs(panel) if panel is not None else None
        if not check(tabs is not None and tabs.count() >= 2, "%s: %s's panel has a second page" % (tag, node.getLabel())):
            return
        page = tabs.widget(1)
        built = [w for w in page.findChildren(QWidget) if w.focusPolicy().value & Qt.FocusPolicy.TabFocus.value]
        report("NOTE %s: %d focusable widgets on page %r before it is shown" % (tag, len(built), tabs.tabText(1)))
        tabs.setCurrentIndex(1)
        page = tabs = panel = None
        built = []
        yield ("sleep", SETTLE_MS)
        panel = panel_of(node)
        tabs = panel_tabs(panel) if panel is not None else None
        page_takers = [w for w in tabs.widget(1).findChildren(QWidget) if takes_tab(w)] if tabs is not None else []
        first = first_in_tab_order(page_takers)
        has_first = first is not None
        if has_first:
            first.setFocus(Qt.TabFocusReason)
        first = None
        page_takers = []
        tabs = panel = None
        if check(has_first, "%s: the page shown late has a widget taking focus" % tag):
            yield ("sleep", KEY_MS)
            xin.chord(["Shift_L", "Tab"])
            yield ("sleep", KEY_MS * 2)
            inside, name = focus_in(node)
            check(inside, "%s: Shift+Tab from the first widget of the page shown late stays in its panel "
                  "(went to %s)" % (tag, name))

    def is_unmodified(p):
        """True when p is a parameter with a labelled row whose value is its default, never animated."""
        if isinstance(p, (NatronEngine.IntParam, NatronEngine.DoubleParam, NatronEngine.ColorParam)):
            same = all(p.getValue(d) == p.getDefaultValue(d) for d in range(p.getNumDimensions()))
        elif isinstance(p, (NatronEngine.ChoiceParam, NatronEngine.BooleanParam, NatronEngine.StringParam)):
            same = p.getValue() == p.getDefaultValue()
        else:
            return False
        return same and not (isinstance(p, NatronEngine.AnimatedParam) and p.getIsAnimated())

    def unmodified_rows_by_page(node):
        """For each page of node, by page label: the labels of its visible, unmodified parameters that
        sit directly on the page, not in a group. A parameter's getParent() is a plain Param wrapper,
        never a PageParam, so the pages are matched by script name."""
        params = node.getParams()
        pages = {}
        for p in params:
            if isinstance(p, NatronEngine.PageParam):
                pages[p.getScriptName()] = p.getLabel()
        found = {}
        for p in params:
            parent = p.getParent()
            page = pages.get(parent.getScriptName()) if parent is not None else None
            if page is None or isinstance(p, NatronEngine.PageParam) or not p.getIsVisible() or not p.getLabel():
                continue
            if is_unmodified(p):
                found.setdefault(page, []).append(p.getLabel())
        return found

    def hide_unmodified_button(panel):
        for b in panel.findChildren(QAbstractButton):
            if b.isCheckable() and HIDE_UNMODIFIED_TIP in b.toolTip():
                return b
        return None

    def tab_index(tabs, label):
        for i in range(tabs.count()):
            if tabs.tabText(i) == label:
                return i
        return -1

    def shown_labels(node, label, texts):
        """Which of texts are the label of a visible row on node's page labelled label."""
        panel = panel_of(node)
        tabs = panel_tabs(panel) if panel is not None else None
        i = tab_index(tabs, label) if tabs is not None else -1
        page = tabs.widget(i) if i >= 0 else None
        shown = set()
        if page is not None:
            for w in page.findChildren(QLabel):
                if gt.class_name(w) == LABEL_CLASS and w.isVisible():
                    shown.add(label_text(w))
            for w in page.findChildren(QAbstractButton):
                if w.isVisible():
                    shown.add(re.sub(r"<[^>]+>", "", w.text()).strip())
        return [t for t in texts if t in shown]

    def click_hide_unmodified(node):
        panel = panel_of(node)
        button = hide_unmodified_button(panel) if panel is not None else None
        found = button is not None
        if found:
            button.click()
        return found

    def unfold_fields(node, dim):
        panel = panel_of(node)
        tip = "<b>%s</b>" % UNFOLD_PARAM
        found = []
        for w in (panel.findChildren(QWidget) if panel is not None else []):
            try:
                if (gt.class_name(w) == "SpinBox" and w.objectName() == "dim-%d" % dim and w.isVisible()
                        and tip in w.toolTip()):
                    found.append(w)
            except RuntimeError:
                continue
        return found

    def unfold_switch_once(node, dims):
        """The row's dimension switch: the checkable button, labelled with the dimension count, beside
        dimension 0's field. Every intermediate widget stays referenced while it is walked, and a
        wrapper that died with the widget under it raises RuntimeError."""
        fields = unfold_fields(node, 0)
        if len(fields) != 1:
            return None
        field = fields[0]
        box = field.parentWidget()
        row = box.parentWidget() if box is not None else None
        buttons = row.findChildren(QAbstractButton) if row is not None else []
        for b in buttons:
            if b.isCheckable() and b.text() == str(dims) and b.isVisible():
                return b
        return None

    def unfold_switch(node, dims):
        """unfold_switch_once, retried after a settle for as long as a wrapper turns up dead."""
        deadline = time.time() + PANEL_TIMEOUT_S
        while True:
            try:
                return unfold_switch_once(node, dims)
            except RuntimeError:
                if time.time() > deadline:
                    raise
            yield ("sleep", SETTLE_MS)

    def unfold_steps():
        tag = "unfold"
        gt.section(tag)
        yield from quiet()
        node = app.createNode(UNFOLD_PLUGIN)
        require(node is not None, "%s: create a %s" % (tag, UNFOLD_PLUGIN), gt.plugin_problem(UNFOLD_PLUGIN))
        node.setPosition(-300, 250)
        param = node.getParam(UNFOLD_PARAM)
        require(param is not None and param.getNumDimensions() >= 3,
                "%s: %s.%s has three dimensions or more" % (tag, UNFOLD_PLUGIN, UNFOLD_PARAM))
        dims = param.getNumDimensions()
        ok = yield from open_panel(node)
        require(ok, "%s: the panel of %s opens" % (tag, node.getLabel()))

        switch = yield from unfold_switch(node, dims)
        require(switch is not None, "%s: %s has a dimension switch" % (tag, UNFOLD_PARAM))
        require(not switch.isChecked(), "%s: %s starts folded" % (tag, UNFOLD_PARAM))
        check(not unfold_fields(node, 2), "%s: folded, %s shows no field for dimension 2" % (tag, UNFOLD_PARAM))
        switch.click()
        switch = None
        yield ("sleep", SETTLE_MS)

        values = [param.getValue(i) for i in range(dims)]
        shown = [unfold_fields(node, i) for i in range(dims)]
        check(all(len(f) == 1 for f in shown), "%s: unfolded, %s shows one field per dimension (%r)" % (
            tag, UNFOLD_PARAM, [len(f) for f in shown]))
        if all(len(f) == 1 for f in shown):
            texts = [f[0].text() for f in shown]
            check(all(abs(float(t) - v) < 1e-6 for t, v in zip(texts, values)),
                  "%s: the fields hold the values %r (show %r)" % (tag, values, texts))
            check(tab_neighbour(shown[1][0], True) is not None and ptr(tab_neighbour(shown[1][0], True)) == ptr(shown[2][0]),
                  "%s: Tab goes from dimension 1's field to dimension 2's" % tag)
            box = shown[2][0]
            box.setFocus()
            box.selectAll()
            QTest.keyClicks(box, str(UNFOLD_VALUE))
            QTest.keyClick(box, Qt.Key_Return)
        shown = None
        box = None
        yield ("sleep", SETTLE_MS)

        after = [param.getValue(i) for i in range(dims)]
        expected = list(values)
        expected[2] = UNFOLD_VALUE
        check(all(abs(a - e) < 1e-6 for a, e in zip(after, expected)),
              "%s: typing %s into dimension 2's field sets that dimension alone (%r, expected %r)" % (
                  tag, UNFOLD_VALUE, after, expected))
        fields = unfold_fields(node, 2)
        check(len(fields) == 1 and abs(float(fields[0].text()) - UNFOLD_VALUE) < 1e-6,
              "%s: dimension 2's field shows %s" % (tag, UNFOLD_VALUE))
        fields = None

    def hide_unmodified_steps():
        tag = "hide unmodified"
        gt.section(tag)
        yield from quiet()
        node = None
        unseen = []
        for pid in UNMODIFIED_PLUGINS:
            node = app.createNode(pid)
            if node is None:
                report("NOTE %s: %s is not available: %s" % (tag, pid, gt.plugin_problem(pid)))
                continue
            node.setPosition(-300, 250)
            rows = unmodified_rows_by_page(node)
            ok = yield from open_panel(node)
            if not check(ok, "%s: the panel of %s opens" % (tag, node.getLabel())):
                node.destroy()
                node = None
                continue
            panel = panel_of(node)
            tabs = panel_tabs(panel) if panel is not None else None
            current = tabs.currentIndex() if tabs is not None else -1
            unseen = [(label, texts) for label, texts in sorted(rows.items())
                      if tabs is not None and tab_index(tabs, label) not in (-1, current)]
            tabs = panel = None
            if unseen:
                break
            report("NOTE %s: %s has no unmodified rows on a page not yet shown" % (tag, pid))
            gt.close_all_panels()
            node.destroy()
            node = None
        if not check(node is not None, "%s: one of %r has unmodified rows on a page not yet shown" % (tag, UNMODIFIED_PLUGINS)):
            return
        label, texts = unseen[0]
        report("NOTE %s: %s page %r, unmodified rows %r" % (tag, node.getPluginID(), label, texts))

        if not check(click_hide_unmodified(node), "%s: the panel has a hide-unmodified button" % tag):
            return
        yield ("sleep", SETTLE_MS)
        panel = panel_of(node)
        tabs = panel_tabs(panel) if panel is not None else None
        if tabs is not None:
            tabs.setCurrentIndex(tab_index(tabs, label))
        tabs = panel = None
        yield ("sleep", SETTLE_MS)
        shown = shown_labels(node, label, texts)
        check(not shown, "%s: the unmodified rows of page %r, shown after hiding unmodified rows, stay hidden "
              "(shown: %r)" % (tag, label, shown))

        click_hide_unmodified(node)
        yield ("sleep", SETTLE_MS)
        shown = shown_labels(node, label, texts)
        missing = [t for t in texts if t not in shown]
        check(not missing, "%s: turning hide-unmodified off shows them again (still hidden: %r)" % (tag, missing))
        gt.close_all_panels()
        node.destroy()
        node = None

    def unbuilt_numeric_param(node, tabs):
        """(param, page label) of a one-dimensional Int or Double parameter with a label, sitting
        directly on a page other than the current one, whose row has no label widget yet."""
        params = node.getParams()
        pages = {}
        for p in params:
            if isinstance(p, NatronEngine.PageParam):
                pages[p.getScriptName()] = p.getLabel()
        current = tabs.currentIndex()
        for p in params:
            if not isinstance(p, (NatronEngine.IntParam, NatronEngine.DoubleParam)) or p.getNumDimensions() != 1:
                continue
            parent = p.getParent()
            page = pages.get(parent.getScriptName()) if parent is not None else None
            if page is None or not p.getIsVisible() or not p.getLabel():
                continue
            i = tab_index(tabs, page)
            if i in (-1, current):
                continue
            if not [w for w in tabs.widget(i).findChildren(QLabel)
                    if gt.class_name(w) == LABEL_CLASS and label_text(w) == p.getLabel()]:
                return p, page
        return None, None

    def has_numeric_on_many_pages(node):
        """Whether the node has more than one page and a visible labelled one-dimensional Int or
        Double parameter directly on a page: only then is its panel worth opening."""
        params = node.getParams()
        pages = set(p.getScriptName() for p in params if isinstance(p, NatronEngine.PageParam))
        if len(pages) < 2:
            return False
        for p in params:
            if not isinstance(p, (NatronEngine.IntParam, NatronEngine.DoubleParam)) or p.getNumDimensions() != 1:
                continue
            parent = p.getParent()
            if parent is not None and parent.getScriptName() in pages and p.getIsVisible() and p.getLabel():
                return True
        return False

    def lazy_page_steps():
        tag = "lazy page"
        gt.section(tag)
        yield from quiet()
        node = param = None
        page = None
        ids = list(UNMODIFIED_PLUGINS)
        for pid in sorted(str(i) for i in NatronEngine.natron.getPluginIDs()):
            if pid not in ids and not any(k in pid.lower() for k in LAZY_SKIP):
                ids.append(pid)
        opened = 0
        for pid in ids[:LAZY_SCAN_LIMIT]:
            node = app.createNode(pid)
            if node is None:
                continue
            if opened >= LAZY_PANEL_LIMIT or not has_numeric_on_many_pages(node):
                node.destroy()
                node = None
                continue
            opened += 1
            node.setPosition(-300, -250)
            ok = yield from open_panel(node)
            panel = panel_of(node) if ok else None
            tabs = panel_tabs(panel) if panel is not None else None
            if tabs is not None:
                param, page = unbuilt_numeric_param(node, tabs)
            tabs = panel = None
            if param is not None:
                break
            report("NOTE %s: %s has no unbuilt numeric row on a page not yet shown" % (tag, pid))
            gt.close_all_panels()
            node.destroy()
            node = None
        if not check(param is not None, "%s: a plug-in has an unbuilt numeric row on a page not yet shown"
                     % tag):
            return
        label = param.getLabel()
        report("NOTE %s: %s page %r, parameter %r" % (tag, node.getPluginID(), page, label))

        before = param.getValue()
        target = param.getDisplayMaximum(0)
        if target == before:
            target = param.getDisplayMinimum(0)
        param.setValue(target)
        after = param.getValue()
        check(after == target, "%s: Python sets %r on the unbuilt row's parameter (%r -> %r, read back %r)"
              % (tag, label, before, target, after))
        check(panel_of(node) is not None and not shown_labels(node, page, [label]),
              "%s: setting the value leaves the row of %r unbuilt" % (tag, label))

        panel = panel_of(node)
        tabs = panel_tabs(panel) if panel is not None else None
        if tabs is not None:
            tabs.setCurrentIndex(tab_index(tabs, page))
        tabs = panel = None
        yield ("sleep", SETTLE_MS)
        check(shown_labels(node, page, [label]) == [label], "%s: showing page %r builds and shows the row of %r"
              % (tag, page, label))
        panel = panel_of(node)
        found = labels_named(panel, label) if panel is not None else []
        _, _, takers = row_of_label(panel, ptr(found[0])) if found else (None, -1, [])
        texts = [w.text() for w in takers if isinstance(w, QLineEdit)]
        found = takers = panel = None
        shown = None
        for t in texts:
            try:
                shown = float(t)
                break
            except ValueError:
                continue
        check(shown is not None and abs(shown - after) <= 1e-2 * max(1.0, abs(after)),
              "%s: the built row's field shows the value Python set (%r, fields %r)" % (tag, after, texts))
        gt.close_all_panels()
        node.destroy()
        node = None

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

        yield from lazy_focus_steps()
        gt.close_all_panels()
        yield from lazy_page_steps()
        gt.close_all_panels()
        yield from hide_unmodified_steps()
        gt.close_all_panels()
        yield from unfold_steps()

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
