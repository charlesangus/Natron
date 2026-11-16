"""Checks that wheel and keyboard zoom in the 2D viewer land on the zoom menu's presets.

Each wheel notch steps one preset (10, 25, 50, 75, 100, 125, 150, 200, 400, 800, 1600, 2400, 3200,
6400%, halving below and doubling above) and keeps the image point under the cursor fixed. Smaller
deltas add up to a notch, turning back discards them, and a level between presets steps to the next
one in the wheel's direction. The viewer's Zoom In and Zoom Out keys and the menu's + and - entries
step along the same presets about the viewer centre.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m422/out GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/m422_viewer_zoom_snap.py build/fast/App/Natron'

Wheel events are synthetic QWheelEvents sent to the viewer's GL widget, keys are QKeyEvents sent to
the viewer tab. The zoom level and the image point under the cursor are read through the viewer's
natronCanonicalToWidget property, whose x scale is the zoom factor.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
RENDER_TIMEOUT_S = 30.0
WATCHDOG_S = 280
SETTLE_MS = 250
STEP_MS = 40
NOTCH = 120
PRESETS = [10, 25, 50, 75, 100, 125, 150, 200, 400, 800, 1600, 2400, 3200, 6400]
ZOOM_MIN_PERCENT = 1
ZOOM_MAX_PERCENT = 102400
FACTOR_TOLERANCE = 1e-6
PIVOT_TOLERANCE = 1e-3

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Q_ARG, Qt
    from PySide6.QtGui import QKeyEvent, QWheelEvent
    from PySide6.QtWidgets import QApplication, QMainWindow, QWidget

    import guitest as gt
    from guitest import check

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    host = app
    # PySide invalidates a child wrapper once the wrapper of the parent it was found through is collected.
    _held = []

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = host.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def viewer_tab():
        tabs = [w for w in gt.widgets_of("ViewerTab") if w.isVisible()]
        if tabs:
            _held.append(tabs[0])
            return tabs[0]
        return None

    def viewer_gl():
        views = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        if views:
            _held.append(views[0])
            return views[0]
        return None

    def zoom_menu():
        tab = viewer_tab()
        for w in tab.findChildren(QWidget) if tab is not None else []:
            try:
                if gt.class_name(w) != "ComboBox":
                    continue
                items = [str(i) for i in (w.property("natronItems") or [])]
                if "Fit" in items and "100%" in items:
                    _held.append(w)
                    return w
            except RuntimeError:
                continue
        return None

    def menu_items():
        menu = zoom_menu()
        return [str(i) for i in (menu.property("natronItems") or [])] if menu is not None else []

    def menu_text():
        menu = zoom_menu()
        return str(menu.property("natronCurrentText") or "").strip() if menu is not None else ""

    def pick_menu_item(text):
        menu = zoom_menu()
        items = menu_items()
        if not check(menu is not None and text in items, "the zoom menu lists %r (got %r)" % (text, items)):
            return
        QMetaObject.invokeMethod(menu, "setCurrentIndex", Q_ARG(int, items.index(text)))
        QApplication.processEvents()

    def mapping():
        """(ox, oy, sx, sy): widget = (ox + sx * x, oy + sy * y) for canonical (x, y), or None."""
        widget = viewer_gl()
        value = widget.property("natronCanonicalToWidget") if widget is not None else None
        if not value or len(value) != 4:
            return None
        m = tuple(float(v) for v in value)
        return m if m[2] != 0. and m[3] != 0. else None

    def percent():
        m = mapping()
        return 100. * m[2] if m is not None else None

    def canonical_at(pos):
        m = mapping()
        if m is None:
            return None
        return ((pos.x() - m[0]) / m[2], (pos.y() - m[1]) / m[3])

    def close(a, b, tolerance=FACTOR_TOLERANCE):
        return a is not None and b is not None and abs(a - b) <= tolerance * max(abs(a), abs(b))

    def label(value):
        return "%g%%" % value

    def menu_label(value):
        return "%d%%" % int(round(value))

    def wheel(delta, pos=None):
        gl = viewer_gl()
        if not check(gl is not None, "the viewer GL widget is up for a wheel event"):
            return
        if pos is None:
            pos = QPoint(gl.width() // 2, gl.height() // 2)
        event = QWheelEvent(QPointF(pos), QPointF(gl.mapToGlobal(pos)), QPoint(), QPoint(0, delta),
                            Qt.NoButton, Qt.NoModifier, Qt.NoScrollPhase, False)
        QApplication.sendEvent(gl, event)
        QApplication.processEvents()

    def send_key(key):
        tab = viewer_tab()
        if not check(tab is not None, "the viewer tab is up for a key event"):
            return
        QApplication.sendEvent(tab, QKeyEvent(QEvent.KeyPress, key, Qt.NoModifier))
        QApplication.sendEvent(tab, QKeyEvent(QEvent.KeyRelease, key, Qt.NoModifier))
        QApplication.processEvents()

    def set_zoom(value):
        gl = viewer_gl()
        if check(gl is not None, "the viewer GL widget is up to set the zoom"):
            QMetaObject.invokeMethod(gl, "zoomSlot", Q_ARG(int, value))
            QApplication.processEvents()

    def fit():
        tab = viewer_tab()
        if tab is not None:
            QMetaObject.invokeMethod(tab, "centerViewer")
            QApplication.processEvents()

    def ladder_above(level):
        """The presets strictly above level, outside the on-preset tolerance."""
        return [p for p in PRESETS if p > level and abs(p - level) >= 0.005 * p]

    def expect_level(value, case, check_menu=True):
        got = percent()
        check(close(got, value), "%s: the zoom is %s (got %r)" % (case, label(value), got))
        if check_menu:
            check(menu_text() == menu_label(value),
                  "%s: the zoom menu shows %s (got %r)" % (case, menu_label(value), menu_text()))

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        win = main_window()
        gt.require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        gt.require(ok, "the main window is active", "an inactive window drops shortcuts and graph clicks")

        viewer = host.getNode("Viewer1") or create("Viewer", 900, 600)
        ok = yield ("until", lambda: viewer_tab() is not None and viewer_gl() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "the viewer tab is up")
        source = create("Constant", 300, 100)
        viewer.connectInput(0, source)
        gt.close_all_panels()
        host.clearSelection()
        yield ("sleep", SETTLE_MS)

        gt.section("menu")
        check(menu_items() == ["Fit", "+", "-"] + ["%d%%" % p for p in PRESETS],
              "the zoom menu lists Fit, +, - and the presets in order (got %r)" % menu_items())

        def fitted():
            fit()
            level = percent()
            return level is not None and not close(level, 100.)

        ok = yield ("until", fitted, RENDER_TIMEOUT_S)
        gt.require(ok, "Fit changes the zoom once the image is shown")
        yield ("sleep", SETTLE_MS)
        fit_level = percent()
        gt.require(fit_level is not None and fit_level < 100., "the image fits below 100%% (got %r)" % fit_level)

        # ---- wheel notches from Fit, about the cursor -----------------------------------------
        gt.section("wheel")
        gl = viewer_gl()
        pivot = QPoint(int(gl.width() * 0.3), int(gl.height() * 0.6))
        up = [p for p in ladder_above(fit_level) if p <= 400]
        check(up[-5:] == [100, 125, 150, 200, 400], "the presets from Fit run up through 100%% to 400%% (got %r)" % up)
        for value in up:
            before = canonical_at(pivot)
            wheel(NOTCH, pivot)
            yield ("sleep", STEP_MS)
            expect_level(value, "wheel in from %s" % label(fit_level))
            after = canonical_at(pivot)
            check(before is not None and after is not None
                  and abs(before[0] - after[0]) < PIVOT_TOLERANCE and abs(before[1] - after[1]) < PIVOT_TOLERANCE,
                  "wheel in to %s keeps the image point under the cursor (%r then %r)" % (label(value), before, after))
        gt.shot("m422-wheel-400.png")
        for value in reversed(up[:-1]):
            before = canonical_at(pivot)
            wheel(-NOTCH, pivot)
            yield ("sleep", STEP_MS)
            expect_level(value, "wheel out")
            after = canonical_at(pivot)
            check(before is not None and after is not None
                  and abs(before[0] - after[0]) < PIVOT_TOLERANCE and abs(before[1] - after[1]) < PIVOT_TOLERANCE,
                  "wheel out to %s keeps the image point under the cursor (%r then %r)" % (label(value), before, after))

        # ---- small deltas add up to a notch; turning back discards them --------------------------
        gt.section("small-deltas")
        set_zoom(100)
        yield ("sleep", STEP_MS)
        for i in range(3):
            wheel(NOTCH // 4)
            yield ("sleep", STEP_MS)
            check(close(percent(), 100.), "30-delta event %d of 4 leaves the zoom at 100%% (got %r)" % (i + 1, percent()))
        wheel(NOTCH // 4)
        yield ("sleep", STEP_MS)
        expect_level(125., "the fourth 30-delta event")
        wheel(NOTCH // 2)
        yield ("sleep", STEP_MS)
        wheel(-NOTCH // 2)
        yield ("sleep", STEP_MS)
        check(close(percent(), 125.), "half a notch in then half out leaves 125%% (got %r)" % percent())
        wheel(-NOTCH // 2)
        yield ("sleep", STEP_MS)
        expect_level(100., "a second half notch out after turning back")
        wheel(2 * NOTCH)
        yield ("sleep", STEP_MS)
        expect_level(150., "one event of two notches from 100%")
        wheel(-3 * NOTCH)
        yield ("sleep", STEP_MS)
        expect_level(75., "one event of three notches out from 150%")

        # ---- levels between presets and the ladder past its ends --------------------------------
        gt.section("off-ladder")
        set_zoom(87)
        yield ("sleep", STEP_MS)
        wheel(NOTCH)
        yield ("sleep", STEP_MS)
        expect_level(100., "wheel in from 87%")
        set_zoom(87)
        yield ("sleep", STEP_MS)
        wheel(-NOTCH)
        yield ("sleep", STEP_MS)
        expect_level(75., "wheel out from 87%")
        set_zoom(10)
        yield ("sleep", STEP_MS)
        wheel(-NOTCH)
        yield ("sleep", STEP_MS)
        expect_level(5., "wheel out from 10%")
        set_zoom(6400)
        yield ("sleep", STEP_MS)
        wheel(NOTCH)
        yield ("sleep", STEP_MS)
        expect_level(12800., "wheel in from 6400%")

        gt.section("bounds")
        for start, delta in ((ZOOM_MAX_PERCENT, NOTCH), (ZOOM_MIN_PERCENT, -NOTCH)):
            set_zoom(start)
            yield ("sleep", STEP_MS)
            before = mapping()
            wheel(delta, pivot)
            yield ("sleep", STEP_MS)
            check(before is not None and mapping() == before,
                  "a notch past %s leaves the view untouched (%r then %r)" % (label(start), before, mapping()))

        # ---- Zoom In and Zoom Out keys and the menu's + and - -----------------------------------
        gt.section("keys")
        set_zoom(87)
        yield ("sleep", STEP_MS)
        send_key(Qt.Key_Plus)
        yield ("sleep", STEP_MS)
        expect_level(100., "Zoom In from 87%")
        send_key(Qt.Key_Plus)
        yield ("sleep", STEP_MS)
        expect_level(125., "Zoom In from 100%")
        set_zoom(100)
        yield ("sleep", STEP_MS)
        send_key(Qt.Key_Minus)
        yield ("sleep", STEP_MS)
        expect_level(75., "Zoom Out from 100%")

        gt.section("menu-actions")
        set_zoom(87)
        yield ("sleep", STEP_MS)
        pick_menu_item("+")
        yield ("sleep", STEP_MS)
        expect_level(100., "the menu's + from 87%")
        pick_menu_item("+")
        yield ("sleep", STEP_MS)
        expect_level(125., "the menu's + from 100%")
        set_zoom(87)
        yield ("sleep", STEP_MS)
        pick_menu_item("-")
        yield ("sleep", STEP_MS)
        expect_level(75., "the menu's - from 87%")
        set_zoom(ZOOM_MAX_PERCENT)
        yield ("sleep", STEP_MS)
        pick_menu_item("+")
        yield ("sleep", STEP_MS)
        expect_level(float(ZOOM_MAX_PERCENT), "the menu's + at the largest zoom")
        set_zoom(100)
        yield ("sleep", STEP_MS)
        gt.shot("m422-menu-100.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
