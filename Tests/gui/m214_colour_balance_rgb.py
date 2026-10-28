"""Screenshots of the ColorBalanceRGB panel for sign-off: its Master, 4 Ways and Masks tabs, the
4 Ways tab with a grade on every wheel, and the Masks tab with each mask output chosen. The panel
must fit a 1920x1080 screen. Shots go to $NATRON_GUI_TEST_OUT as colorbalancergb_<tab>.png.

The 4 Ways tab is a 2x2 grid of wheels (angle = hue, radius = chroma), each with its title above,
its luminance slider beneath and a value readout, and the knobs' rows in a collapsed Values group.
In the default layout the tab must fit the panel without a vertical scrollbar. A drag on the lift
wheel must change liftHue and liftChroma, and one undo must restore both.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m214_colour_balance_rgb.py <Natron binary or AppImage>
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
PLUGIN_ID = "ColorBalanceRGB"
TABS = (("Master", "master"), ("4 Ways", "4ways"), ("Masks", "masks"))
WHEELS = ("offset", "lift", "gain", "power")
WHEEL_TITLES = ("Global Offset", "Shadows Lift", "Highlights Gain", "Power")
WHEEL_GRADE = {
    "offset": (0.01, 200.0, 0.005),
    "lift": (0.2, 30.0, 0.2),
    "gain": (-0.1, 90.0, 0.1),
    "power": (0.1, 300.0, 0.05),
}
OUTPUTS = (("shadowsMask", 1), ("midtonesMask", 2), ("highlightsMask", 3))
SCREEN_W = 1920
SCREEN_H = 1080

try:
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, QRect, Qt
    from PySide6.QtGui import QGuiApplication, QMouseEvent
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLabel, QLineEdit, QScrollArea, QTabWidget, QWidget

    import guitest as gt
    from guitest import check, report, require

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return hold(panel)
                except RuntimeError:
                    continue
        return None

    def show_tab(panel, text):
        for tabs in panel.findChildren(QTabWidget):
            for i in range(tabs.count()):
                if tabs.tabText(i).replace("&", "") == text:
                    tabs.setCurrentIndex(i)
                    QApplication.processEvents()
                    return True
        return False

    def visible_labels(panel):
        return [w.text() for w in panel.findChildren(QLabel) if w.isVisible() and w.text().strip()]

    def enlarge(panel):
        for area in panel.findChildren(QScrollArea):
            content = area.widget()
            if content is not None:
                area.setMinimumHeight(min(content.sizeHint().height() + 4, SCREEN_H - 120))
        QApplication.processEvents()

    def screen_crop(name, panel):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(panel.mapToGlobal(panel.rect().topLeft()), panel.size())
        rect = rect.intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(gt.OUT, name + ".png")
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def wheel_of(panel, wheel):
        name = "ColorWheel_%sChroma" % wheel
        for w in panel.findChildren(QWidget):
            if gt.class_name(w) == "ColorWheelWidget" and w.objectName() == name:
                return w
        return None

    def child_named(panel, name):
        for w in panel.findChildren(QWidget):
            if w.objectName() == name:
                return hold(w)
        return None

    def scroll_area_of(panel, widget):
        """The QScrollArea inside panel that holds widget, found by walking widget's parents."""
        w = widget.parentWidget()
        while w is not None and w is not panel:
            if isinstance(w, QScrollArea):
                return hold(w)
            w = w.parentWidget()
        for area in panel.findChildren(QScrollArea):
            content = area.widget()
            if content is not None and content.isAncestorOf(widget):
                return hold(area)
        return None

    def parent_chain(widget):
        names = []
        w = widget
        while w is not None:
            names.append("%s(%s)" % (gt.class_name(w), w.objectName()))
            w = w.parentWidget()
        return " < ".join(names)

    def send_mouse(widget, kind, local, button, buttons):
        local = QPointF(local)
        ev = QMouseEvent(kind, local, QPointF(widget.mapToGlobal(local)), button, buttons, Qt.NoModifier)
        QApplication.sendEvent(widget, ev)

    def drag(widget, start, delta, steps=8):
        send_mouse(widget, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        QApplication.processEvents()
        for k in range(1, steps + 1):
            p = QPoint(start.x() + delta.x() * k // steps, start.y() + delta.y() * k // steps)
            send_mouse(widget, QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton)
            QApplication.processEvents()
        end = QPoint(start.x() + delta.x(), start.y() + delta.y())
        send_mouse(widget, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()

    def click_panel_undo(panel):
        for b in panel.findChildren(QAbstractButton):
            if "Undo the last change" in b.toolTip() and b.isEnabled():
                b.click()
                QApplication.processEvents()
                return True
        return False

    def steps():
        gt.section("colorbalancergb")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        gt.close_all_panels()
        node = app.createNode(PLUGIN_ID)
        require(node is not None, "create ColorBalanceRGB", gt.plugin_problem(PLUGIN_ID))
        node.setPosition(0, 0)
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open ColorBalanceRGB's settings panel")
        yield ("sleep", 800)
        panel = panel_of(node)
        require(panel is not None, "find ColorBalanceRGB's settings panel")

        # Before enlarge(): the tab must fit the panel as the default layout sizes it.
        check(show_tab(panel, "4 Ways"), "the panel has a 4 Ways tab")
        yield ("sleep", 300)
        grid = child_named(panel, "ColorWheelGrid_wheels")
        if check(grid is not None and grid.isVisible(), "the 4 Ways tab shows the wheel grid"):
            area = scroll_area_of(panel, grid)
            if area is None:
                report("NOTE wheel grid parents: " + parent_chain(grid))
            if check(area is not None, "the wheel grid sits in the panel's scroll area"):
                bar = area.verticalScrollBar()
                report("NOTE 4 Ways viewport %dx%d, content %dx%d, scroll max %d" % (
                    area.viewport().width(), area.viewport().height(),
                    area.widget().width(), area.widget().height(), bar.maximum()))
                check(not bar.isVisible() and bar.maximum() == 0,
                      "the 4 Ways tab fits the panel without a vertical scrollbar")
            cells = [child_named(panel, "ColorWheelCell_%sChroma" % w) for w in WHEELS]
            if check(all(c is not None for c in cells), "the grid has a cell per wheel"):
                pos = [grid.mapFromGlobal(c.mapToGlobal(QPoint(0, 0))) for c in cells]
                check(pos[0].y() == pos[1].y() and pos[2].y() == pos[3].y() and pos[0].y() < pos[2].y()
                      and pos[0].x() < pos[1].x() and pos[2].x() < pos[3].x(),
                      "the wheels are laid out two to a row (offset, lift / gain, power)")
        screen_crop("colorbalancergb_4ways_default", panel)
        enlarge(panel)
        yield ("sleep", 300)

        screen = QGuiApplication.primaryScreen().geometry()
        report("NOTE screen %dx%d" % (screen.width(), screen.height()))
        rect = QRect(panel.mapToGlobal(panel.rect().topLeft()), panel.size())
        check(rect.width() <= SCREEN_W and rect.height() <= SCREEN_H,
              "the panel fits 1920x1080 (got %dx%d)" % (rect.width(), rect.height()))

        for text, name in TABS:
            check(show_tab(panel_of(node), text), "the panel has a %s tab" % text)
            yield ("sleep", 300)
            screen_crop("colorbalancergb_" + name, panel_of(node))

        gt.section("wheels")
        show_tab(panel_of(node), "4 Ways")
        yield ("sleep", 300)
        for wheel in WHEELS:
            w = wheel_of(panel_of(node), wheel)
            if check(w is not None, "the 4 Ways tab has a %s wheel" % wheel):
                report("NOTE %s wheel is %dx%d" % (wheel, w.width(), w.height()))
        lift_hue = node.getParam("liftHue")
        lift_chroma = node.getParam("liftChroma")
        require(lift_hue is not None and lift_chroma is not None, "ColorBalanceRGB has liftHue and liftChroma")
        wheel = wheel_of(panel_of(node), "lift")
        require(wheel is not None, "found the lift wheel")
        h0, c0 = lift_hue.getValue(), lift_chroma.getValue()
        centre = QPoint(wheel.width() // 2, wheel.height() // 2)
        drag(wheel, centre, QPoint(30, -30))
        yield ("sleep", 400)
        h1, c1 = lift_hue.getValue(), lift_chroma.getValue()
        report("NOTE lift wheel drag: hue %r -> %r, chroma %r -> %r" % (h0, h1, c0, c1))
        check(c1 > c0, "a lift wheel drag away from the centre raises liftChroma")
        check(h1 != h0, "a lift wheel drag up and to the right changes liftHue")
        check(0.0 < h1 < 90.0, "the drag's angle (up and to the right) lands liftHue between 0 and 90 degrees (got %r)" % h1)
        screen_crop("colorbalancergb_4ways_wheel_dragged", panel_of(node))
        check(click_panel_undo(panel_of(node)), "the panel's undo button is enabled after the drag")
        yield ("sleep", 400)
        h2, c2 = lift_hue.getValue(), lift_chroma.getValue()
        report("NOTE after one undo: hue %r, chroma %r" % (h2, c2))
        check(h2 == h0 and c2 == c0, "one undo restores liftHue and liftChroma (the drag is one undo step)")

        for wheel in WHEELS:
            luminance, hue, chroma = WHEEL_GRADE[wheel]
            for suffix, value in (("Luminance", luminance), ("Hue", hue), ("Chroma", chroma)):
                param = node.getParam(wheel + suffix)
                require(param is not None, "ColorBalanceRGB has %s%s" % (wheel, suffix))
                param.setValue(value)
        show_tab(panel_of(node), "4 Ways")
        yield ("sleep", 400)
        labels = visible_labels(panel_of(node))
        for title in WHEEL_TITLES + ("Values",):
            check(any(t.startswith(title) for t in labels), "the 4 Ways tab shows %s" % title)
        for wheel in WHEELS:
            luminance, hue, chroma = WHEEL_GRADE[wheel]
            readout = child_named(panel_of(node), "ColorWheelReadout_%sChroma" % wheel)
            text = readout.text() if readout is not None else ""
            report("NOTE %s readout: %s" % (wheel, text))
            check(("C %.4f" % chroma) in text and ("Y %.3f" % luminance) in text,
                  "the %s readout follows the knobs (got %r)" % (wheel, text))
        screen_crop("colorbalancergb_4ways_graded", panel_of(node))

        output = node.getParam("output")
        require(output is not None, "ColorBalanceRGB has an output knob")
        show_tab(panel_of(node), "Masks")
        for name, index in OUTPUTS:
            output.setValue(index)
            yield ("sleep", 300)
            screen_crop("colorbalancergb_masks_" + name, panel_of(node))
        output.setValue(0)
        node.destroy()
        yield ("sleep", 200)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
