"""Knob cleanup and Keyer redesign.

Sections:
  - toggle-sync: Alt+U flips the Render menu's Interactive Updates action and the
    renderOnEditingFinished preference together, and setting the preference flips the action back.
  - transform-drag: dragging a Transform's centre handle in the viewer re-renders during the drag
    with interactive updates on, and only on release with them off.
  - no-overlay-knobs: Transform, RotoPaint, CornerPin and Ramp have no visible interactive or hidpi
    knob (a native node has none at all; an OFX plug-in's are hidden and host-driven).
  - footer-order: panel shots of Grade, Saturation, ColorLookup and an OFX colour node, with Clamp
    Black and Clamp White side by side above Mix.
  - keyer-matte: Keyer on a ramp and on a synthetic greenscreen, the matte shown in the viewer.

Screenshots go to $NATRON_GUI_TEST_OUT. Usage: Tests/gui/run-gui-test.sh Tests/gui/m95_knobs_keyer.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
SETTLE_MS = 600
DRAG_STEPS = 12
PIXEL_DIFF_MIN = 40
CHANGED_PIXELS_MIN = 100

PLUGIN_VIEWER = "fr.inria.built-in.Viewer"
PLUGIN_CHECKER = "net.sf.openfx.CheckerBoardPlugin"
PLUGIN_CONSTANT = "net.sf.openfx.ConstantPlugin"
PLUGIN_TRANSFORM = "net.sf.openfx.TransformPlugin"
PLUGIN_CORNERPIN = "CornerPin"
PLUGIN_ROTOPAINT = "fr.inria.built-in.RotoPaint"
PLUGIN_RAMP = "net.sf.openfx.Ramp"
PLUGIN_GRADE = "net.sf.openfx.GradePlugin"
PLUGIN_SATURATION = "net.sf.openfx.SaturationPlugin"
PLUGIN_COLORLOOKUP = "net.sf.openfx.ColorLookupPlugin"
# OFX colour plug-ins with clamp, premult, mask and mix; the first one registered is used.
OFX_FOOTER_CANDIDATES = ("net.sf.openfx.HSVToolPlugin", "net.sf.openfx.ColorTransferPlugin", "net.sf.openfx.Quantize")
PLUGIN_KEYER = "net.sf.openfx.KeyerPlugin"
OVERLAY_KNOBS = ("interactive", "hidpi", "transformInteractive", "rampInteractive", "RotoTransformInteractive")
UNTONED_VIEW = "Un-tone-mapped"

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, QRect, Qt, Q_ARG
    from PySide6.QtGui import QAction, QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QMainWindow, QWidget

    import guitest as gt
    from guitest import check, report

    _held = []

    def hold(w):
        """Keeps a wrapper alive for the rest of the run: child wrappers die with their parent's."""
        if w is not None:
            _held.append(w)
        return w

    # ---- graph helpers -------------------------------------------------------------------

    def create(plugin_id, x=0, y=0):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        gt.require(p is not None, "%s has a parameter %r" % (node.getPluginID(), name))
        return p

    def connect(node, input_nb, source):
        if node.getInput(input_nb) is not None:
            node.disconnectInput(input_nb)
        return gt.require(node.connectInput(input_nb, source), "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def destroy_all(nodes):
        for n in nodes:
            try:
                n.destroy(False)
            except Exception:
                report("NOTE could not destroy %r: %s" % (n, traceback.format_exc().splitlines()[-1]))
        QApplication.processEvents()

    def viewer_node():
        v = app.getNode("Viewer1")
        if v is None:
            v = create(PLUGIN_VIEWER, 0, 800)
        return v

    def show(node):
        connect(viewer_node(), 0, node)

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return hold(w)
        return None

    def activate():
        win = main_window()
        if win is None:
            return False
        win.activateWindow()
        win.raise_()
        QApplication.processEvents()
        return win.isActiveWindow()

    # ---- viewer helpers ------------------------------------------------------------------

    def visible_viewer():
        found = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return hold(found[0]) if found else None

    def viewer_tab():
        found = [w for w in gt.widgets_of("ViewerTab") if w.isVisible()]
        return hold(found[0]) if found else None

    def fit_viewer():
        tab = viewer_tab()
        if tab is not None:
            QMetaObject.invokeMethod(tab, "centerViewer")
        QApplication.processEvents()

    def select_untoned_view():
        """The viewer's view combo set to the un-tone-mapped view, so screen values follow the image."""
        tab = viewer_tab()
        if tab is None:
            return False
        for combo in tab.findChildren(QWidget):
            try:
                if gt.class_name(combo) != "ComboBox":
                    continue
                items = [str(i) for i in (combo.property("natronItems") or [])]
            except RuntimeError:
                continue
            if UNTONED_VIEW in items:
                hold(combo)
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, items.index(UNTONED_VIEW)))
                QApplication.processEvents()
                return True
        return False

    def viewer_crop():
        widget = visible_viewer()
        if widget is None:
            return None
        QApplication.processEvents()
        top_left = widget.mapToGlobal(widget.rect().topLeft())
        return QGuiApplication.primaryScreen().grabWindow(0).toImage().copy(top_left.x(), top_left.y(), widget.width(), widget.height())

    def changed_pixels(a, b, stride=3):
        if a is None or b is None or a.size() != b.size():
            return -1
        count = 0
        for y in range(0, a.height(), stride):
            for x in range(0, a.width(), stride):
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                if abs(ca.red() - cb.red()) + abs(ca.green() - cb.green()) + abs(ca.blue() - cb.blue()) > PIXEL_DIFF_MIN:
                    count += 1
        return count

    def send_mouse(widget, etype, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), QPointF(widget.mapToGlobal(pos.toPoint())), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    # ---- panel helpers -------------------------------------------------------------------

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLabel) + panel.findChildren(QWidget):
                try:
                    if hasattr(w, "text") and w.text() == label:
                        return hold(panel)
                except (RuntimeError, TypeError):
                    continue
        return None

    def open_panel(node):
        gt.close_all_panels()
        app.selectNode(node, True)
        graphs = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        if graphs:
            QMetaObject.invokeMethod(hold(graphs[0]), "showSelectedNodeSettingsPanel")
        QApplication.processEvents()

    def panel_crop(name, panel):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(panel.mapToGlobal(panel.rect().topLeft()), panel.size()).intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if screen.copy(rect).save(path) else "NOTE could not save ") + path)

    def label_widget(panel, text):
        found = []
        for w in panel.findChildren(QWidget):
            try:
                if w.isVisible() and hasattr(w, "text") and w.text().strip().rstrip(":") == text:
                    found.append(w)
            except (RuntimeError, TypeError):
                continue
        return hold(found[0]) if found else None

    def top_of(w):
        return w.mapToGlobal(w.rect().topLeft()).y()

    # ---- sections ------------------------------------------------------------------------

    def interactive_action():
        win = main_window()
        if win is None:
            return None
        for a in win.findChildren(QAction):
            try:
                if a.text().replace("&", "") == "Interactive Updates":
                    return hold(a)
            except RuntimeError:
                continue
        return None

    def section_toggle_sync():
        gt.section("toggle-sync")
        pref = NatronEngine.natron.getSettings().getParam("renderOnEditingFinished")
        gt.require(pref is not None, "Settings has renderOnEditingFinished")
        action = interactive_action()
        gt.require(action is not None, "the main window has the Interactive Updates action")
        check(action.isCheckable(), "Interactive Updates is checkable")
        check(action.isChecked() == (not pref.get()), "the action starts in step with the preference")
        before = pref.get()
        check(activate(), "the main window is active before Alt+U")
        QTest.keyClick(main_window(), Qt.Key_U, Qt.AltModifier)
        yield ("sleep", 300)
        check(pref.get() != before, "Alt+U flips the preference (%s to %s)" % (before, pref.get()))
        check(action.isChecked() == (not pref.get()), "Alt+U leaves the action in step with the preference")
        pref.set(before)
        yield ("sleep", 300)
        check(action.isChecked() == (not before), "setting the preference flips the action back")
        action.trigger()
        yield ("sleep", 300)
        check(pref.get() != before, "triggering the action from the Render menu flips the preference")
        pref.set(before)
        yield ("sleep", 200)

    def drag_renders(transform, interactive):
        """Drags the Transform's centre handle; returns (pixels changed mid-drag, after release, moved)."""
        pref = NatronEngine.natron.getSettings().getParam("renderOnEditingFinished")
        pref.set(not interactive)
        translate = param(transform, "translate")
        translate.set(0.0, 0.0)
        yield ("sleep", SETTLE_MS * 2)
        widget = visible_viewer()
        before = viewer_crop()
        start = QPointF(widget.width() / 2.0, widget.height() / 2.0)
        end = start + QPointF(widget.width() * 0.15, 0.0)
        send_mouse(widget, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, DRAG_STEPS + 1):
            send_mouse(widget, QEvent.MouseMove, start + (end - start) * (i / float(DRAG_STEPS)), Qt.NoButton, Qt.LeftButton)
            yield ("sleep", 30)
        yield ("sleep", SETTLE_MS * 2)
        mid = viewer_crop()
        gt.shot("transform_drag_%s_mid.png" % ("on" if interactive else "off"))
        send_mouse(widget, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        yield ("sleep", SETTLE_MS * 2)
        after = viewer_crop()
        moved = abs(translate.get().x) > 1.0
        return changed_pixels(before, mid), changed_pixels(before, after), moved

    def section_transform_drag():
        gt.section("transform-drag")
        pref = NatronEngine.natron.getSettings().getParam("renderOnEditingFinished")
        saved = pref.get()
        checker = create(PLUGIN_CHECKER, 0, 0)
        transform = create(PLUGIN_TRANSFORM, 0, 200)
        connect(transform, 0, checker)
        show(transform)
        open_panel(transform)
        yield ("sleep", SETTLE_MS)
        fit_viewer()
        check(activate(), "the main window is active before the drag")
        for interactive in (True, False):
            mode = "on" if interactive else "off"
            mid, after, moved = yield from drag_renders(transform, interactive)
            check(moved, "interactive %s: the drag moves the centre handle (translate.x %.1f)" % (mode, param(transform, "translate").get().x))
            check(after >= CHANGED_PIXELS_MIN, "interactive %s: the image has moved after release (%d pixels)" % (mode, after))
            if interactive:
                check(mid >= CHANGED_PIXELS_MIN, "interactive on: the viewer re-renders during the drag (%d pixels)" % mid)
            else:
                check(mid < CHANGED_PIXELS_MIN, "interactive off: the viewer holds the image until release (%d pixels)" % mid)
        pref.set(saved)
        gt.close_all_panels()
        destroy_all([transform, checker])

    def section_no_overlay_knobs():
        gt.section("no-overlay-knobs")
        for plugin_id, native in ((PLUGIN_TRANSFORM, True), (PLUGIN_ROTOPAINT, True), (PLUGIN_CORNERPIN, True), (PLUGIN_RAMP, False)):
            if plugin_id not in gt._plugin_ids:
                check(False, "%s is registered" % plugin_id)
                continue
            node = create(plugin_id, 400, 0)
            for name in OVERLAY_KNOBS:
                p = node.getParam(name)
                if native:
                    check(p is None, "%s has no %s knob" % (plugin_id, name))
                elif p is not None:
                    check(not p.getIsVisible() and not p.getIsPersistent(), "%s's %s is hidden and not saved" % (plugin_id, name))
            open_panel(node)
            ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
            check(ok, "%s's panel opens" % plugin_id)
            if ok:
                panel = panel_of(node)
                for text in ("Interactive", "HiDPI", "Interactive Update"):
                    check(label_widget(panel, text) is None, "%s's panel shows no %s row" % (plugin_id, text))
                panel_crop("panel_%s" % plugin_id.split(".")[-1], panel)
            gt.close_all_panels()
            destroy_all([node])
            yield ("sleep", 200)

    def section_footer_order():
        gt.section("footer-order")
        ofx = [p for p in OFX_FOOTER_CANDIDATES if p in gt._plugin_ids]
        check(bool(ofx), "an OFX colour plug-in with a footer is registered (%s)" % ", ".join(OFX_FOOTER_CANDIDATES))
        for plugin_id in (PLUGIN_GRADE, PLUGIN_SATURATION, PLUGIN_COLORLOOKUP) + tuple(ofx[:1]):
            node = create(plugin_id, 600, 0)
            open_panel(node)
            ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
            check(ok, "%s's panel opens" % plugin_id)
            if ok:
                yield ("sleep", SETTLE_MS)
                panel = panel_of(node)
                black = label_widget(panel, "Clamp Black")
                white = label_widget(panel, "Clamp White")
                mix = label_widget(panel, "Mix")
                check(black is not None and white is not None and mix is not None, "%s shows Clamp Black, Clamp White and Mix" % plugin_id)
                if black is not None and white is not None and mix is not None:
                    check(abs(top_of(black) - top_of(white)) < 6, "%s: Clamp Black and Clamp White share a row (y %d, %d)" % (plugin_id, top_of(black), top_of(white)))
                    check(top_of(black) < top_of(mix), "%s: the clamps sit above Mix" % plugin_id)
                panel_crop("footer_%s" % plugin_id.split(".")[-1], panel)
            gt.close_all_panels()
            destroy_all([node])
            yield ("sleep", 200)

    def section_keyer_matte():
        gt.section("keyer-matte")
        check(select_untoned_view(), "the viewer's %s view is selected" % UNTONED_VIEW)
        keyer = create(PLUGIN_KEYER, 800, 200)
        if PLUGIN_RAMP in gt._plugin_ids:
            ramp = create(PLUGIN_RAMP, 800, 0)
            connect(keyer, 0, ramp)
            show(keyer)
            fit_viewer()
            yield ("sleep", SETTLE_MS * 3)
            gt.shot("keyer_ramp.png")
            report("NOTE keyer_ramp.png: the matte should rise and fall across the ramp as the trapezoid range sets")
        else:
            check(False, "%s is registered" % PLUGIN_RAMP)
            ramp = None

        # A synthetic greenscreen: green boxes keyed against skin-tone boxes.
        checker = create(PLUGIN_CHECKER, 1000, 0)
        param(checker, "color0").set(0.1, 0.7, 0.15, 1.0)
        param(checker, "color1").set(0.8, 0.55, 0.45, 1.0)
        param(checker, "color2").set(0.8, 0.55, 0.45, 1.0)
        param(checker, "color3").set(0.1, 0.7, 0.15, 1.0)
        connect(keyer, 0, checker)
        # getOptions() returns the option IDs. A pure green key colour gives the green boxes 0.7 and
        # the skin boxes 0.55, so a matte rising from 0.6 to 0.65 keeps only the green.
        source = param(keyer, "keySource")
        source.setValue(source.getOptions().index("color"))
        param(keyer, "keyColor").set(0.0, 1.0, 0.0, 1.0) if param(keyer, "keyColor").getNumDimensions() == 4 else param(keyer, "keyColor").set(0.0, 1.0, 0.0)
        matte_range = param(keyer, "range")
        for i, v in enumerate((0.6, 0.65, 1e6, 1e6)):
            matte_range.setValue(v, i)
        check([round(matte_range.getValue(i), 3) for i in range(4)] == [0.6, 0.65, 1e6, 1e6], "the Keyer range reads back as set")
        show(keyer)
        fit_viewer()
        yield ("sleep", SETTLE_MS * 3)
        crop = viewer_crop()
        gt.shot("keyer_greenscreen.png")
        check(crop is not None, "the viewer shows the keyed greenscreen")
        report("NOTE keyer_greenscreen.png: the matte (alpha only) should separate the green boxes from the skin-tone boxes")
        destroy_all([n for n in (keyer, ramp, checker) if n is not None])

    def steps():
        viewer_node()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        gt.require(ok, "the viewer widget is up")
        gt.require(main_window() is not None, "found the main window")
        gt.close_all_panels()
        yield ("sleep", 1000)
        check(select_untoned_view(), "the viewer's %s view is selected" % UNTONED_VIEW)
        for name, runner in (("toggle-sync", section_toggle_sync), ("transform-drag", section_transform_drag), ("no-overlay-knobs", section_no_overlay_knobs),
                             ("footer-order", section_footer_order), ("keyer-matte", section_keyer_matte)):
            report("NOTE running section " + name)
            try:
                yield from runner()
            except SystemExit:
                raise
            except Exception:
                check(False, "section %s raised: %s" % (name, traceback.format_exc()))
                gt.shot("error-%s.png" % name)
            gt.close_all_panels()
            yield ("sleep", SETTLE_MS)

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
