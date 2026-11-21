import math
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
WHEEL_PER_DELTA = 1.00152
SNAP_BAND = 0.08
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

    def wheel_snap(raw, delta):
        nxt = min(ZOOM_MAX_PERCENT, max(ZOOM_MIN_PERCENT, raw * WHEEL_PER_DELTA ** delta))
        band = math.log1p(SNAP_BAND)
        start, end = math.log(raw), math.log(nxt)
        jumped = None
        for p in PRESETS:
            at = math.log(p)
            if abs(end - at) <= band:
                return nxt, float(p)
            over = (start < at - band and end > at + band) or (start > at + band and end < at - band)
            if over and (jumped is None or abs(end - at) < abs(end - math.log(jumped))):
                jumped = float(p)
        if jumped is not None:
            return jumped, jumped
        return nxt, nxt

    def keeps_pivot(before, after):
        return (before is not None and after is not None
                and abs(before[0] - after[0]) < PIVOT_TOLERANCE and abs(before[1] - after[1]) < PIVOT_TOLERANCE)

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

        gt.section("wheel")
        gl = viewer_gl()
        pivot = QPoint(int(gl.width() * 0.3), int(gl.height() * 0.6))
        raw, level = fit_level, fit_level
        visited = []
        while level < 400. and len(visited) < 40:
            raw, level = wheel_snap(raw, NOTCH)
            before = canonical_at(pivot)
            wheel(NOTCH, pivot)
            yield ("sleep", STEP_MS)
            expect_level(level, "wheel in from %s" % label(fit_level))
            check(keeps_pivot(before, canonical_at(pivot)),
                  "wheel in to %s keeps the image point under the cursor" % label(level))
            visited.append(level)
        check(all(p in visited for p in (100., 125., 150., 200., 400.)),
              "wheel in from Fit lands on 100, 125, 150, 200 and 400%% (got %r)" % visited)
        check(any(all(not close(v, p) for p in PRESETS) for v in visited),
              "wheel in from Fit also shows levels between the presets (got %r)" % visited)
        gt.shot("m422-wheel-400.png")
        for _ in range(len(visited)):
            raw, level = wheel_snap(raw, -NOTCH)
            before = canonical_at(pivot)
            wheel(-NOTCH, pivot)
            yield ("sleep", STEP_MS)
            expect_level(level, "wheel out")
            check(keeps_pivot(before, canonical_at(pivot)),
                  "wheel out to %s keeps the image point under the cursor" % label(level))

        gt.section("smooth")
        set_zoom(100)
        yield ("sleep", STEP_MS)
        raw, level = 100., 100.
        for i in range(4):
            raw, level = wheel_snap(raw, NOTCH)
            wheel(NOTCH)
            yield ("sleep", STEP_MS)
            expect_level(level, "notch %d in from 100%%" % (i + 1))
        check(close(percent(), 200.), "four notches in from 100%% end on 200%% (got %r)" % percent())
        set_zoom(100)
        yield ("sleep", STEP_MS)
        wheel(NOTCH)
        wheel(NOTCH)
        wheel(NOTCH)
        yield ("sleep", STEP_MS)
        between = percent()
        check(between is not None and 160. < between < 185. and all(not close(between, p) for p in PRESETS),
              "the third notch from 100%% stops between 150%% and 200%% (got %r)" % between)

        gt.section("sticky")
        # Through the menu: zoomSlot() alone leaves the menu on the previous level, and a step
        # that stays on 100% does not refresh it.
        pick_menu_item("100%")
        yield ("sleep", STEP_MS)
        wheel(NOTCH // 4)
        yield ("sleep", STEP_MS)
        expect_level(100., "a quarter notch in stays on the 100% preset")
        wheel(NOTCH // 4)
        yield ("sleep", STEP_MS)
        expect_level(100. * WHEEL_PER_DELTA ** (NOTCH // 2), "a second quarter notch leaves 100% smoothly")
        wheel(-NOTCH // 4)
        yield ("sleep", STEP_MS)
        expect_level(100., "a quarter notch back snaps onto 100% again")
        set_zoom(91)
        yield ("sleep", STEP_MS)
        wheel(NOTCH)
        yield ("sleep", STEP_MS)
        expect_level(100., "a notch from 91% that would jump past 100% lands on it")

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
        expect_level(10. / WHEEL_PER_DELTA ** NOTCH, "wheel out from 10%")
        set_zoom(6400)
        yield ("sleep", STEP_MS)
        wheel(NOTCH)
        yield ("sleep", STEP_MS)
        expect_level(6400. * WHEEL_PER_DELTA ** NOTCH, "wheel in from 6400%")

        gt.section("bounds")
        for start, delta in ((ZOOM_MAX_PERCENT, NOTCH), (ZOOM_MIN_PERCENT, -NOTCH)):
            set_zoom(start)
            yield ("sleep", STEP_MS)
            before = mapping()
            wheel(delta, pivot)
            yield ("sleep", STEP_MS)
            check(before is not None and mapping() == before,
                  "a notch past %s leaves the view untouched (%r then %r)" % (label(start), before, mapping()))

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
