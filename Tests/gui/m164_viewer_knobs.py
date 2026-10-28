"""GUI check that each Viewer node keeps its own view state in knobs (M164.P3.T1).

Graph: Constant "cA" (0.4, 0.1, 0.2) and Constant "cB" (0.1, 0.3, 0.4) feed Viewer1 (A = input 0,
B = input 1) and Viewer2 (the same inputs, A = input 0, B = input 1, but with Python later switching
its A). Viewer2 sits in a pane of its own, so both viewers are on screen at once.

What is checked, in order:
  baseline     every viewer knob is reachable with getParam; the two viewers start with equal knobs;
  toolbar      Viewer2's Display menu, then Viewer1's gamma box (typed), Display Channels, wipe
               operator, View menu and gain slider (dragged) change only that viewer's knobs; the
               other viewer's knobs and screen pixels do not move;
  gain undo    the gain slider drag, which makes many knob writes, is one Edit > Undo step, and redo
               brings it back;
  roi          a region of interest built with the mouse on Viewer1 (press, moves, release) is one
               undo step: it returns the RoI knobs to the rectangle that was there before the drag;
  python       run from the Script Editor, so each run is one undo entry: Viewer2.setAInput(1) turns
               the image from cB to cA, and Edit > Undo and Redo switch it back and forth on screen
               (the engine reacts to undo); parameter sets on operation, wipeAngle and wipeCenter
               move the toolbar's wipe operator menu and the wipe handle (read from which input the
               screen shows at the two ends of the handle's travel), undo restores the screen; a
               displayChannels set moves the channels menu; the wipe handle is then brought on
               screen and a screenshot of both viewers is taken;
  layer        a layer and an alpha channel the input lacks are kept on Viewer2 and shown in its
               layer menu;
  save/reload  the project is saved, reset and loaded again: every knob of each viewer and each
               toolbar widget is back, each viewer shows the pixels it showed, and Viewer1's
               state did not leak into Viewer2.

Not covered: the zoom (it is not a knob; the ViewerGL zoom factor is not readable from Python) and the
wipe handle's own drawing (it is not exposed to Python; the screen pixels and a screenshot stand in).

Run through Tests/gui/run-gui-test.sh at 1920x1080. Screenshots (m164-*.png) and results.txt go to
$NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
LOAD_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 400
RENDER_MS = 700
# Screen pixels are 8-bit and pass through the viewer's display transform.
COLOUR_SLACK = 4
FAR = 100000.0
HALF_PI = 1.5707963267948966

PLUGIN_CONSTANT = "net.sf.openfx.ConstantPlugin"
VIEWER_PLUGIN = "fr.inria.built-in.Viewer"
V1 = "Viewer1"
V2 = "Viewer2"
MISSING_LAYER = "m164Layer"

KNOBS = [
    "enableGain", "gain", "enableGamma", "gamma", "autoContrast", "enableCheckerBoard",
    "display", "view", "look", "displayChannels", "displayChannelsB", "layer", "alphaChannel",
    "aInput", "bInput", "operation", "wipeCenter", "wipeAmount", "wipeAngle", "activeView",
    "enableFps", "desiredFps", "inPoint", "outPoint", "enableRegionOfInterest", "userRoIBottomLeft",
    "userRoISize", "clipToFormat", "fullFrame", "enableDownscale", "downscaleLevel",
]
ROI_KNOBS = ["enableRegionOfInterest", "userRoIBottomLeft", "userRoISize"]

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Q_ARG, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QMainWindow, QPlainTextEdit, QWidget

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    PROJECT_PATH = os.path.join(gt.OUT, "m164_viewer_knobs.ntpz")

    # ---- plumbing --------------------------------------------------------------------------

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def edit_menu_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().startswith(prefix):
                        return a
        return None

    def trigger(prefix):
        a = edit_menu_action(prefix)
        if not check(a is not None and a.isEnabled(), "an enabled %s action is in the Edit menu" % prefix):
            return False
        a.trigger()
        QApplication.processEvents()
        return True

    def run_in_script_editor(script):
        """Puts script into the Script Editor and runs it the way its Execute button does, so the
        run is recorded as one undo entry."""
        editors = gt.widgets_of("ScriptEditor")
        editor = next((e for e in editors if e.isVisible()), editors[0] if editors else None)
        if not check(editor is not None, "found a Script Editor"):
            return False
        inputs = [w for w in editor.findChildren(QPlainTextEdit) if gt.class_name(w) == "InputScriptTextEdit"]
        if not check(len(inputs) == 1, "the Script Editor has one input field (got %d)" % len(inputs)):
            return False
        inputs[0].setPlainText(script)
        QMetaObject.invokeMethod(editor, "onExecScriptClicked")
        QApplication.processEvents()
        return True

    def pane_names():
        names = []
        for w in gt.widgets_of("TabWidget"):
            try:
                if w.isVisible() and w.objectName():
                    names.append(w.objectName())
            except RuntimeError:
                continue
        return sorted(names)

    _tab_holders = []
    _tabs = {}

    def tab_for(name):
        """The ViewerTab of the viewer node name, made the current tab of its pane."""
        cached = _tabs.get(name)
        if cached is not None:
            try:
                cached.isVisible()
                return cached
            except RuntimeError:
                _tabs.pop(name, None)
        for pane in pane_names():
            tw = app.getTabWidget(pane)
            if tw is None:
                continue
            for i in range(tw.count()):
                if name in tw.getTabLabel(i):
                    if tw.getCurrentIndex() != i:
                        tw.setCurrentIndex(i)
                        QApplication.processEvents()
                    widget = tw.currentWidget()
                    if widget is None or gt.class_name(widget) != "ViewerTab":
                        continue
                    _tab_holders.append(tw)
                    _tabs[name] = widget
                    return widget
        return None

    def forget_tabs():
        _tabs.clear()

    def children_of(tab, class_names, needle=None):
        found = []
        for w in tab.findChildren(QWidget):
            try:
                if gt.class_name(w) in class_names and (needle is None or needle in (w.toolTip() or "")):
                    found.append(w)
            except RuntimeError:
                continue
        return found

    def widget_of(name, class_names, needle):
        tab = tab_for(name)
        found = children_of(tab, class_names, needle) if tab is not None else []
        return found[0] if found else None

    def viewer_gl(name):
        tab = tab_for(name)
        found = children_of(tab, ("ViewerGL",)) if tab is not None else []
        return found[0] if found else None

    def combo_items(combo):
        return [str(i) for i in (combo.property("natronItems") or [])]

    def combo_current(combo):
        return str(combo.property("natronCurrentText") or "")

    def combo_with_item(name, item):
        tab = tab_for(name)
        for c in (children_of(tab, ("ComboBox",)) if tab is not None else []):
            if item in combo_items(c):
                return c
        return None

    def set_combo_index(combo, index):
        QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
        QApplication.processEvents()

    def choose_other(combo, preferred=()):
        """Selects an item whose text contains one of preferred, else the first one that is not the
        current item. Returns the selected text, or None when there is nothing to change to."""
        items = combo_items(combo)
        current = combo_current(combo)
        for want in preferred:
            for i, it in enumerate(items):
                if want.lower() in it.lower() and it != current:
                    set_combo_index(combo, i)
                    return it
        for i, it in enumerate(items):
            if it != current:
                set_combo_index(combo, i)
                return it
        return None

    def mouse(widget, kind, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(kind, QPointF(pos), QPointF(widget.mapToGlobal(pos)), button,
                                                   buttons, Qt.NoModifier))

    def drag(widget, start, end, steps=8):
        mouse(widget, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, steps + 1):
            p = QPoint(start.x() + (end.x() - start.x()) * i // steps, start.y() + (end.y() - start.y()) * i // steps)
            mouse(widget, QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton)
            QApplication.processEvents()
        mouse(widget, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()

    def type_into(box, text):
        box.setFocus()
        box.selectAll()
        QTest.keyClicks(box, text)
        QTest.keyClick(box, Qt.Key_Return)
        QApplication.processEvents()

    def spin_value(box):
        try:
            return float(box.text())
        except ValueError:
            return None

    # ---- knobs -----------------------------------------------------------------------------

    def knob_value(node, name):
        p = node.getParam(name)
        if p is None:
            return None
        if hasattr(p, "getLayer"):
            return p.getLayer()
        v = p.get()
        if hasattr(v, "x") and hasattr(v, "y"):
            return (v.x, v.y)
        return v

    def snap(node):
        return dict((k, knob_value(node, k)) for k in KNOBS)

    def same_value(a, b):
        if isinstance(a, tuple) and isinstance(b, tuple):
            return len(a) == len(b) and all(same_value(x, y) for x, y in zip(a, b))
        if isinstance(a, float) or isinstance(b, float):
            try:
                return abs(float(a) - float(b)) <= 1e-6 * max(1.0, abs(float(a)), abs(float(b)))
            except (TypeError, ValueError):
                return False
        return a == b

    def differences(a, b, only=None):
        return [k for k in (only or KNOBS) if not same_value(a.get(k), b.get(k))]

    def roi_of(node):
        s = snap(node)
        return dict((k, s[k]) for k in ROI_KNOBS)

    # ---- pixels ----------------------------------------------------------------------------

    def px(gl):
        """Two 5x5-averaged screen colours either side of the middle of the viewer, read from a
        screen grab since a widget grab of the GL viewer can come out blank under llvmpipe."""
        QApplication.processEvents()
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        out = []
        for fx in (0.35, 0.65):
            c = gl.mapToGlobal(QPoint(int(gl.width() * fx), int(gl.height() * 0.5)))
            total = [0, 0, 0]
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    p = image.pixelColor(c.x() + dx, c.y() + dy)
                    total[0] += p.red()
                    total[1] += p.green()
                    total[2] += p.blue()
            out.append(tuple(t // 25 for t in total))
        return tuple(out)

    def same_px(a, b):
        if a is None or b is None:
            return False
        return all(abs(x - y) <= COLOUR_SLACK for pa, pb in zip(a, b) for x, y in zip(pa, pb))

    def brightness(p):
        return sum(sum(c) for c in p)

    def viewer_px(name):
        gl = viewer_gl(name)
        return px(gl) if gl is not None else None

    def settle_px(name):
        """Waits until two grabs RENDER_MS apart agree and returns the last one, or None."""
        previous = None
        for _ in range(int(POLL_TIMEOUT_S * 1000 / RENDER_MS)):
            yield ("sleep", RENDER_MS)
            now = viewer_px(name)
            if now is not None and same_px(now, previous):
                return now
            previous = now
        return None

    def px_until(name, predicate):
        ok = yield ("until", lambda: predicate(viewer_px(name)), POLL_TIMEOUT_S)
        # A late refresh must not replace what was reached.
        yield ("sleep", RENDER_MS)
        return ok and predicate(viewer_px(name))

    def knobs_until(predicate):
        return (yield ("until", predicate, POLL_TIMEOUT_S))

    # ---- the steps -------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: bool(gt.widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()
        app_var = "app%d" % (app.getAppID() + 1)

        gt.section("setup")
        app.getProjectParam("frameRange").set(1, 30)
        cA = create(PLUGIN_CONSTANT, 0, 0)
        cA.getParam("color").set(0.4, 0.1, 0.2, 1.0)
        cB = create(PLUGIN_CONSTANT, 200, 0)
        cB.getParam("color").set(0.1, 0.3, 0.4, 1.0)
        viewer1 = app.getNode(V1) or create(VIEWER_PLUGIN, 0, 200)
        viewer2 = create(VIEWER_PLUGIN, 200, 200)
        require(viewer1.getScriptName() == V1, "the first viewer is " + V1)
        require(viewer2.getScriptName() == V2, "the second viewer is %s (got %s)" % (V2, viewer2.getScriptName()))
        for viewer in (viewer1, viewer2):
            require(viewer.connectInput(0, cA), "cA feeds %s input 0" % viewer.getScriptName())
            require(viewer.connectInput(1, cB), "cB feeds %s input 1" % viewer.getScriptName())
        gt.close_all_panels()
        py1 = app.getViewer(V1)
        py2 = app.getViewer(V2)
        require(py1 is not None and py2 is not None, "both viewers have a Python wrapper")
        py1.setAInput(0)
        py1.setBInput(1)
        py2.setAInput(0)
        py2.setBInput(1)

        ok = yield ("until", lambda: tab_for(V1) is not None and tab_for(V2) is not None, POLL_TIMEOUT_S)
        require(ok, "both viewers have a tab")
        home = None
        for pane in pane_names():
            tw = app.getTabWidget(pane)
            if tw is not None and any(V1 in tw.getTabLabel(i) for i in range(tw.count())):
                home = pane
        require(home is not None, "Viewer1's tab is in a pane")
        split = app.getTabWidget(home).splitHorizontally()
        require(split is not None, "split the pane " + home)
        require(app.moveTab(V2, split), "move %s into a pane of its own" % V2)
        split = None
        forget_tabs()
        ok = yield ("until", lambda: viewer_gl(V1) is not None and viewer_gl(V1).isVisible()
                    and viewer_gl(V2) is not None and viewer_gl(V2).isVisible(), POLL_TIMEOUT_S)
        require(ok, "both viewers are on screen")

        gt.section("baseline")
        missing = [k for k in KNOBS if viewer1.getParam(k) is None or viewer2.getParam(k) is None]
        check(not missing, "getParam reaches every viewer knob on both viewers (missing: %r)" % (missing,))
        base1 = snap(viewer1)
        base2 = snap(viewer2)
        report("NOTE Viewer1 knobs at creation: %r" % (base1,))
        check(not differences(base1, base2, [k for k in KNOBS if k not in ("aInput", "bInput")]),
              "the two viewers start with equal knobs (differ: %r)" % differences(base1, base2))
        check(base1["aInput"] == 0 and base1["bInput"] == 1, "Python setAInput/setBInput set Viewer1's inputs (got %r, %r)"
              % (base1["aInput"], base1["bInput"]))
        check(knob_value(viewer1, "enableGain") is False and knob_value(viewer1, "gain") == 0.0,
              "gain starts off at 0 f-stops")
        p1 = yield from settle_px(V1)
        p2 = yield from settle_px(V2)
        require(p1 is not None and p2 is not None, "both viewers settle")
        check(same_px(p1, p2), "both viewers show the same image to start with")
        gt.shot("m164-baseline.png")

        # ---- Viewer2's Display menu ----------------------------------------------------------
        gt.section("toolbar-viewer2")
        display2 = widget_of(V2, ("ComboBox",), "display device")
        require(display2 is not None, "found Viewer2's Display menu")
        report("NOTE Display menu items %r, current %r" % (combo_items(display2), combo_current(display2)))
        chosen = choose_other(display2)
        if chosen is None:
            report("NOTE only one display in the config; Viewer2's Display cannot change")
        else:
            ok = yield from knobs_until(lambda: knob_value(viewer2, "display") != base2["display"])
            check(ok, "choosing %r in Viewer2's Display menu writes its display knob (got %r)"
                  % (chosen, knob_value(viewer2, "display")))
            check(chosen in str(knob_value(viewer2, "display")) or str(knob_value(viewer2, "display")) in chosen,
                  "the display knob names the chosen display")
        snap2 = snap(viewer2)
        check(not differences(snap(viewer1), base1), "Viewer1's knobs did not move with Viewer2's display (%r)"
              % differences(snap(viewer1), base1))
        p2 = yield from settle_px(V2)
        require(p2 is not None, "Viewer2 settles")

        # ---- Viewer1's toolbar ---------------------------------------------------------------
        gt.section("toolbar-viewer1")
        gamma_box = widget_of(V1, ("SpinBox",), "Viewer gamma correction level")
        require(gamma_box is not None, "found Viewer1's gamma box")
        type_into(gamma_box, "1.8")
        ok = yield from knobs_until(lambda: abs((knob_value(viewer1, "gamma") or 0.0) - 1.8) < 1e-6)
        check(ok, "typing 1.8 in the gamma box writes gamma (got %r)" % (knob_value(viewer1, "gamma"),))
        check(knob_value(viewer1, "enableGamma") is True, "a gamma other than 1 turns the gamma toggle on")

        channels1 = widget_of(V1, ("ComboBox",), "Display Channels:")
        require(channels1 is not None, "found Viewer1's Display Channels menu")
        set_combo_index(channels1, 2)
        ok = yield from knobs_until(lambda: knob_value(viewer1, "displayChannels") == 2)
        check(ok, "choosing R in Viewer1's Display Channels writes displayChannels (got %r)"
              % (knob_value(viewer1, "displayChannels"),))

        operator1 = combo_with_item(V1, "W-Over")
        require(operator1 is not None, "found Viewer1's wipe operator menu")
        set_combo_index(operator1, 1)
        ok = yield from knobs_until(lambda: knob_value(viewer1, "operation") == 1)
        check(ok, "choosing the first wipe in Viewer1's operator menu writes operation (got %r)"
              % (knob_value(viewer1, "operation"),))

        view1 = widget_of(V1, ("ComboBox",), "How the image is rendered")
        require(view1 is not None, "found Viewer1's View menu")
        report("NOTE View menu items %r, current %r" % (combo_items(view1), combo_current(view1)))
        view_before = knob_value(viewer1, "view")
        view_chosen = choose_other(view1, preferred=("un-tone", "raw"))
        if view_chosen is None:
            report("NOTE only one view; Viewer1's View cannot change")
        else:
            ok = yield from knobs_until(lambda: knob_value(viewer1, "view") != view_before)
            check(ok, "choosing %r in Viewer1's View menu writes its view knob (got %r)"
                  % (view_chosen, knob_value(viewer1, "view")))

        check(not differences(snap(viewer2), snap2), "Viewer2's knobs did not move with Viewer1's toolbar (%r)"
              % differences(snap(viewer2), snap2))
        yield ("sleep", RENDER_MS)
        q2 = yield from settle_px(V2)
        check(same_px(q2, p2), "Viewer2's pixels did not move with Viewer1's toolbar")
        p1_wiped = yield from settle_px(V1)
        check(p1_wiped is not None and not same_px(p1_wiped, p1), "Viewer1's pixels changed with its own toolbar")

        # ---- gain drag, one undo step --------------------------------------------------------
        gt.section("gain-undo")
        gain_slider = widget_of(V1, ("ScaleSliderQWidget",), "Gain is shown as f-stops")
        gain_box = widget_of(V1, ("SpinBox",), "Gain is shown as f-stops")
        require(gain_slider is not None and gain_box is not None, "found Viewer1's gain slider and box")
        before_gain = snap(viewer1)
        w, h = gain_slider.width(), gain_slider.height()
        drag(gain_slider, QPoint(w // 2, h // 2), QPoint(w * 3 // 4, h // 2), steps=12)
        ok = yield from knobs_until(lambda: (knob_value(viewer1, "gain") or 0.0) > 1.0)
        gain_after = knob_value(viewer1, "gain")
        check(ok, "dragging the gain slider right raises gain (got %r)" % (gain_after,))
        check(knob_value(viewer1, "enableGain") is True, "a gain other than 0 turns the gain toggle on")
        check(abs((spin_value(gain_box) or 0.0) - gain_after) < 0.01, "the gain box follows the knob (%r vs %r)"
              % (spin_value(gain_box), gain_after))
        check(not differences(snap(viewer2), snap2), "Viewer2's knobs did not move with Viewer1's gain")
        bright = yield from settle_px(V1)
        check(bright is not None and brightness(bright) > brightness(p1_wiped), "Viewer1 is brighter with gain")

        require(trigger("undo"), "undo the gain drag")
        ok = yield from knobs_until(lambda: knob_value(viewer1, "enableGain") is False)
        check(ok and abs(knob_value(viewer1, "gain") or 0.0) < 1e-6,
              "one undo restores the gain before the drag (enableGain %r, gain %r)"
              % (knob_value(viewer1, "enableGain"), knob_value(viewer1, "gain")))
        check(abs(spin_value(gain_box) or 0.0) < 0.01, "the gain box follows the undo (%r)" % (spin_value(gain_box),))
        check(not differences(snap(viewer1), before_gain), "one undo restored every Viewer1 knob (%r)"
              % differences(snap(viewer1), before_gain))
        back = yield from settle_px(V1)
        check(same_px(back, p1_wiped), "Viewer1 shows the un-gained image after the undo")
        require(trigger("redo"), "redo the gain drag")
        ok = yield from knobs_until(lambda: abs((knob_value(viewer1, "gain") or 0.0) - gain_after) < 1e-6)
        check(ok, "redo brings the dragged gain back (got %r)" % (knob_value(viewer1, "gain"),))

        # ---- RoI drag, one undo step ---------------------------------------------------------
        gt.section("roi")
        tab1 = tab_for(V1)
        gl1 = viewer_gl(V1)
        QMetaObject.invokeMethod(tab1, "onCreateNewRoIPressed")
        QApplication.processEvents()
        ok = yield from knobs_until(lambda: knob_value(viewer1, "enableRegionOfInterest") is True)
        require(ok, "the new-RoI action turns the RoI on")
        roi0 = roi_of(viewer1)
        report("NOTE RoI before the drag %r" % (roi0,))
        W, H = gl1.width(), gl1.height()
        drag(gl1, QPoint(int(W * 0.3), int(H * 0.3)), QPoint(int(W * 0.6), int(H * 0.6)), steps=10)
        ok = yield from knobs_until(lambda: not same_value(roi_of(viewer1)["userRoISize"], roi0["userRoISize"]))
        roi1 = roi_of(viewer1)
        report("NOTE RoI after the drag %r" % (roi1,))
        check(ok, "dragging out a region of interest changes the RoI knobs")
        check(roi1["enableRegionOfInterest"] is True, "the RoI stays on after the drag")
        require(trigger("undo"), "undo the RoI drag")
        ok = yield from knobs_until(lambda: not differences(roi_of(viewer1), roi0, ROI_KNOBS))
        check(ok, "one undo returns the RoI knobs to the rectangle before the drag (now %r, want %r)"
              % (roi_of(viewer1), roi0))
        require(trigger("redo"), "redo the RoI drag")
        ok = yield from knobs_until(lambda: not differences(roi_of(viewer1), roi1, ROI_KNOBS))
        check(ok, "redo brings the dragged RoI back")
        check(not differences(snap(viewer2), snap2), "Viewer2's knobs did not move with Viewer1's RoI")
        snap1 = snap(viewer1)
        gt.shot("m164-viewer1-roi.png")

        # ---- Python edits on Viewer2 ---------------------------------------------------------
        gt.section("python-input")
        before_a = yield from settle_px(V2)
        require(before_a is not None, "Viewer2 settles before the input switch")
        require(run_in_script_editor('%s.getViewer("%s").setAInput(1)\n' % (app_var, V2)),
                "run Viewer2.setAInput(1) in the Script Editor")
        ok = yield from knobs_until(lambda: knob_value(viewer2, "aInput") == 1)
        check(ok, "setAInput(1) writes aInput (got %r)" % (knob_value(viewer2, "aInput"),))
        ok = yield from px_until(V2, lambda p: p is not None and not same_px(p, before_a))
        switched = viewer_px(V2)
        check(ok, "the viewer re-renders to show input 1 after setAInput(1)")
        require(trigger("undo"), "undo the input switch")
        ok = yield from knobs_until(lambda: knob_value(viewer2, "aInput") == 0)
        check(ok, "undo puts aInput back to 0 (got %r)" % (knob_value(viewer2, "aInput"),))
        ok = yield from px_until(V2, lambda p: same_px(p, before_a))
        check(ok, "undo makes the engine render input 0 again (screen %r, want %r)" % (viewer_px(V2), before_a))
        require(trigger("redo"), "redo the input switch")
        ok = yield from px_until(V2, lambda p: same_px(p, switched))
        check(ok and knob_value(viewer2, "aInput") == 1, "redo makes the engine render input 1 again")
        require(trigger("undo"), "undo the input switch again, leaving Viewer2 on input 0")
        ok = yield from px_until(V2, lambda p: same_px(p, before_a))
        check(ok and knob_value(viewer2, "aInput") == 0, "Viewer2 is back on input 0")

        gt.section("python-wipe")
        script = ('n = %s.getNode("%s")\n'
                  'n.getParam("operation").set(1)\n'
                  'n.getParam("wipeAngle").set(%r)\n'
                  'n.getParam("wipeCenter").set(%r, %r)\n') % (app_var, V2, HALF_PI, -FAR, -FAR)
        require(run_in_script_editor(script), "run the Viewer2 wipe edit in the Script Editor")
        ok = yield from knobs_until(lambda: knob_value(viewer2, "operation") == 1)
        check(ok, "the Python operation set reaches the knob")
        check(same_value(knob_value(viewer2, "wipeCenter"), (-FAR, -FAR)), "wipeCenter holds the Python value (got %r)"
              % (knob_value(viewer2, "wipeCenter"),))
        operator2 = combo_with_item(V2, "W-Over")
        require(operator2 is not None, "found Viewer2's wipe operator menu")
        ok = yield ("until", lambda: combo_current(operator2) not in ("", combo_items(operator2)[0]), POLL_TIMEOUT_S)
        check(ok, "the operator menu moves with the Python set (shows %r)" % (combo_current(operator2),))
        wipe_far_neg = yield from settle_px(V2)
        require(wipe_far_neg is not None, "Viewer2 settles with the wipe at the far corner")
        check(not same_px(wipe_far_neg, before_a), "the wipe shows the B input where it was showing A")
        check(same_px(wipe_far_neg[:1] * 2, wipe_far_neg), "the whole image is on one side of the handle's line")
        check(not differences(snap(viewer1), snap1), "Viewer1's knobs did not move with Viewer2's Python edit (%r)"
              % differences(snap(viewer1), snap1))

        require(run_in_script_editor('%s.getNode("%s").getParam("wipeCenter").set(%r, %r)\n'
                                     % (app_var, V2, FAR, FAR)), "run the wipe move in the Script Editor")
        ok = yield from px_until(V2, lambda p: p is not None and not same_px(p, wipe_far_neg))
        wipe_far_pos = viewer_px(V2)
        check(ok, "moving the wipe handle to the opposite corner flips which input the screen shows")
        check(same_px(wipe_far_pos, before_a), "the far side of the handle shows input A")
        require(trigger("undo"), "undo the wipe move")
        ok = yield from px_until(V2, lambda p: same_px(p, wipe_far_neg))
        check(ok and same_value(knob_value(viewer2, "wipeCenter"), (-FAR, -FAR)),
              "undo moves the wipe handle back and the engine re-renders")

        require(run_in_script_editor('%s.getNode("%s").getParam("displayChannels").set(3)\n' % (app_var, V2)),
                "run the Viewer2 channels edit in the Script Editor")
        channels2 = widget_of(V2, ("ComboBox",), "Display Channels:")
        require(channels2 is not None, "found Viewer2's Display Channels menu")
        ok = yield ("until", lambda: combo_current(channels2) in ("G", "Green"), POLL_TIMEOUT_S)
        check(ok, "the channels menu moves to G with the Python set (shows %r)" % (combo_current(channels2),))
        check(knob_value(viewer1, "displayChannels") == 2, "Viewer1 keeps R")

        home_center = base2["wipeCenter"]
        require(run_in_script_editor('%s.getNode("%s").getParam("wipeCenter").set(%r, %r)\n'
                                     % (app_var, V2, home_center[0], home_center[1])),
                "run the wipe handle back to the middle in the Script Editor")
        ok = yield from knobs_until(lambda: same_value(knob_value(viewer2, "wipeCenter"), home_center))
        check(ok, "the wipe handle is back at the middle")
        yield from settle_px(V2)
        gt.shot("m164-two-viewers.png")

        # ---- a layer the input lacks ---------------------------------------------------------
        gt.section("layer")
        layer_before = knob_value(viewer2, "layer")
        alpha_before = knob_value(viewer2, "alphaChannel")
        viewer2.getParam("layer").setLayer(MISSING_LAYER)
        viewer2.getParam("alphaChannel").set(MISSING_LAYER + ".A")
        yield ("sleep", SETTLE_MS)
        check(MISSING_LAYER in str(knob_value(viewer2, "layer")),
              "a layer the input lacks is kept (got %r)" % (knob_value(viewer2, "layer"),))
        check(knob_value(viewer2, "alphaChannel") == MISSING_LAYER + ".A",
              "an alpha channel the input lacks is kept (got %r)" % (knob_value(viewer2, "alphaChannel"),))
        listed = any(MISSING_LAYER in " ".join(combo_items(c) + [combo_current(c)])
                     for c in children_of(tab_for(V2), ("ComboBox",)))
        check(listed, "Viewer2's layer menu shows the layer as an unlisted entry")
        check(knob_value(viewer1, "layer") == snap1["layer"], "Viewer1's layer did not move")

        # ---- save and reload -----------------------------------------------------------------
        gt.section("save-reload")
        saved1 = snap(viewer1)
        saved2 = snap(viewer2)
        check(differences(saved1, saved2), "the viewers' knobs differ from each other before saving")
        report("NOTE Viewer1 saved %r" % (saved1,))
        report("NOTE Viewer2 saved %r" % (saved2,))
        saved_px1 = yield from settle_px(V1)
        saved_px2 = yield from settle_px(V2)
        require(app.saveProjectAs(PROJECT_PATH), "save the project to " + PROJECT_PATH)
        require(app.resetProject(), "reset the project (close it, keep the window)")
        forget_tabs()
        yield ("sleep", SETTLE_MS)
        require(app.loadProject(PROJECT_PATH) is not None, "reopen " + PROJECT_PATH)
        ok = yield ("until", lambda: app.getNode(V1) is not None and app.getNode(V2) is not None, LOAD_TIMEOUT_S)
        require(ok, "both viewers are back after reopening")
        yield ("sleep", SETTLE_MS)
        gt.close_all_panels()
        viewer1 = app.getNode(V1)
        viewer2 = app.getNode(V2)
        got1 = snap(viewer1)
        got2 = snap(viewer2)
        check(not differences(got1, saved1), "Viewer1's knobs are restored (differ: %r)" % differences(got1, saved1))
        check(not differences(got2, saved2), "Viewer2's knobs are restored (differ: %r)" % differences(got2, saved2))
        for k in differences(got1, saved1):
            report("NOTE Viewer1 %s saved %r loaded %r" % (k, saved1[k], got1[k]))
        for k in differences(got2, saved2):
            report("NOTE Viewer2 %s saved %r loaded %r" % (k, saved2[k], got2[k]))
        check(got1["gamma"] == saved1["gamma"] and got2["gamma"] == saved2["gamma"] and got1["gamma"] != got2["gamma"],
              "each viewer reloads its own gamma")

        ok = yield ("until", lambda: tab_for(V1) is not None and tab_for(V2) is not None, POLL_TIMEOUT_S)
        require(ok, "both viewers have a tab after reopening")
        home = None
        for pane in pane_names():
            tw = app.getTabWidget(pane)
            if tw is not None and any(V1 in tw.getTabLabel(i) for i in range(tw.count())):
                home = pane
        require(home is not None, "Viewer1's tab is in a pane after reopening")
        if home is not None and any(V2 in app.getTabWidget(home).getTabLabel(i)
                                    for i in range(app.getTabWidget(home).count())):
            split = app.getTabWidget(home).splitHorizontally()
            require(split is not None, "split the pane " + home)
            require(app.moveTab(V2, split), "move %s into a pane of its own again" % V2)
            split = None
            forget_tabs()
        ok = yield ("until", lambda: viewer_gl(V1) is not None and viewer_gl(V1).isVisible()
                    and viewer_gl(V2) is not None and viewer_gl(V2).isVisible(), POLL_TIMEOUT_S)
        require(ok, "both viewers are on screen after reopening")

        gamma_box = widget_of(V1, ("SpinBox",), "Viewer gamma correction level")
        gamma_box2 = widget_of(V2, ("SpinBox",), "Viewer gamma correction level")
        gain_box = widget_of(V1, ("SpinBox",), "Gain is shown as f-stops")
        check(abs((spin_value(gamma_box) or 0.0) - 1.8) < 0.01, "Viewer1's gamma box shows 1.8 (%r)" % (spin_value(gamma_box),))
        check(abs((spin_value(gamma_box2) or 0.0) - 1.0) < 0.01, "Viewer2's gamma box shows 1 (%r)" % (spin_value(gamma_box2),))
        check(abs((spin_value(gain_box) or 0.0) - gain_after) < 0.01, "Viewer1's gain box shows the gain (%r vs %r)"
              % (spin_value(gain_box), gain_after))
        channels1 = widget_of(V1, ("ComboBox",), "Display Channels:")
        channels2 = widget_of(V2, ("ComboBox",), "Display Channels:")
        check(channels1 is not None and combo_current(channels1) in ("R", "Red"),
              "Viewer1's channels menu shows R (%r)" % (combo_current(channels1) if channels1 else None))
        check(channels2 is not None and combo_current(channels2) in ("G", "Green"),
              "Viewer2's channels menu shows G (%r)" % (combo_current(channels2) if channels2 else None))
        operator1 = combo_with_item(V1, "W-Over")
        operator2 = combo_with_item(V2, "W-Over")
        check(operator1 is not None and operator2 is not None and combo_current(operator1) == combo_current(operator2)
              and combo_current(operator1) != combo_items(operator1)[0],
              "both operator menus show a wipe (%r, %r)" % (combo_current(operator1) if operator1 else None,
                                                             combo_current(operator2) if operator2 else None))
        view1 = widget_of(V1, ("ComboBox",), "How the image is rendered")
        check(view1 is not None and (not got1["view"] or got1["view"] in combo_current(view1)
                                     or combo_current(view1) in got1["view"]),
              "Viewer1's View menu shows its saved view (%r)" % (combo_current(view1) if view1 else None))

        now1 = yield from settle_px(V1)
        now2 = yield from settle_px(V2)
        check(same_px(now1, saved_px1), "Viewer1 shows the pixels it showed before saving")
        check(same_px(now2, saved_px2), "Viewer2 shows the pixels it showed before saving")

        viewer2.getParam("layer").setLayer(layer_before)
        viewer2.getParam("alphaChannel").set(alpha_before)
        yield ("sleep", SETTLE_MS)
        yield from settle_px(V2)
        gt.shot("m164-two-viewers-reloaded.png")
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
