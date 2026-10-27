"""The native HSVKeyer panel draws its three ranges (outer low, inner low, centre, inner high and
outer high fields, in that order) as trapezoid strips above their spinboxes, each with Smooth,
Recentre and Reset Zoom buttons, below the Use toggles. The model defaults to HSV. The S button
toggles the range's smooth knob and the strip redraws its falloffs as curves. Reset Zoom shows 0 to
1 for saturation and luma, zooming out to take in a point outside. Setting the source colour sets
each range's centre to the colour's component and keeps its offsets; switching the model does the
same in the new model. A range's row, S button included, greys out while its Use toggle is off, and
the strips grow with the panel. Screenshots go to $NATRON_GUI_TEST_OUT as hsvkeyer_hsv.png (the
defaults), hsvkeyer_oklch.png, hsvkeyer_smooth.png, hsvkeyer_hue_off.png and hsvkeyer_wide.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m122_hsvkeyer.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
PLUGIN_ID = "HSVKeyer"
RANGE_KNOBS = ("hueRange", "saturationRange", "lumaRange")
TOGGLE_LABELS = ("Use Hue", "Use Saturation", "Use Luma")
STRIP_PREFIX = "TrapezoidStrip_"
SMOOTH_KNOBS = ("hueSmooth", "saturationSmooth", "lumaSmooth")
SMOOTH_PREFIX = "TrapezoidSmooth_"
RECENTRE_PREFIX = "TrapezoidRecentre_"
RESET_ZOOM_PREFIX = "TrapezoidResetZoom_"
BUTTON_PREFIXES = (SMOOTH_PREFIX, RECENTRE_PREFIX, RESET_ZOOM_PREFIX)
DIMENSION_NAMES = ("Outer Low", "Inner Low", "Centre", "Inner High", "Outer High")
CENTRE_DIM = 2
OFFSET_DIMS = (0, 1, 3, 4)
# Orange: hue 30 degrees in HSV and HSL, full saturation, V 1 and L 0.5.
SOURCE = (1.0, 0.5, 0.0)
SOURCE_HUE = 30.0 / 360.0

try:
    from PySide6.QtCore import QMetaObject, QPoint, QPointF, QRect, Qt
    from PySide6.QtGui import QGuiApplication, QWheelEvent
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLabel, QLineEdit, QScrollArea, QSplitter, QWidget

    import guitest as gt
    from guitest import check, report, require

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
                        return panel
                except RuntimeError:
                    continue
        return None

    def strip_of(panel, knob):
        if panel is None:
            return None
        strips = [s for s in panel.findChildren(QWidget, STRIP_PREFIX + knob) if s.isVisible()]
        return strips[0] if len(strips) == 1 else None

    def all_strips_shown(panel):
        return all(strip_of(panel, k) is not None for k in RANGE_KNOBS)

    def top_of(widget):
        return widget.mapToGlobal(widget.rect().topLeft()).y()

    def toggle_label(panel, text):
        labels = [w for w in panel.findChildren(QLabel) if w.isVisible() and w.text().strip() == text]
        return labels[0] if labels else None

    def button_of(panel, prefix, knob):
        buttons = [b for b in panel.findChildren(QAbstractButton, prefix + knob) if b.isVisible()]
        return buttons[0] if len(buttons) == 1 else None

    def widen(panel, extra):
        """Widens the panel by extra pixels through its splitter, or its window when it has none."""
        w = panel.parentWidget()
        child = panel
        while w is not None:
            if isinstance(w, QSplitter) and w.orientation() == Qt.Orientation.Horizontal:
                sizes = w.sizes()
                i = w.indexOf(child)
                if i >= 0:
                    others = [j for j in range(len(sizes)) if j != i and sizes[j] > extra]
                    if others:
                        sizes[i] += extra
                        sizes[others[0]] -= extra
                        w.setSizes(sizes)
                        QApplication.processEvents()
                        return "splitter"
            child = w
            w = w.parentWidget()
        window = panel.window()
        window.resize(window.width() + extra, window.height())
        QApplication.processEvents()
        return "window"

    def enlarge(panel):
        for area in panel.findChildren(QScrollArea):
            content = area.widget()
            if content is not None:
                area.setMinimumHeight(content.sizeHint().height() + 4)
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

    def centre(param):
        return param.getValue(CENTRE_DIM)

    def offsets(param):
        return [param.getValue(i) for i in OFFSET_DIMS]

    def set_range(param, values):
        for i, v in enumerate(values):
            param.setValue(v, i)

    def dimension_rows(panel):
        """The dimension labels' texts, left to right, for each row of them, top to bottom."""
        rows = {}
        for w in panel.findChildren(QLabel):
            if w.isVisible() and w.text().strip() in DIMENSION_NAMES:
                pos = w.mapToGlobal(w.rect().topLeft())
                rows.setdefault(pos.y() // 4, []).append((pos.x(), w.text().strip()))
        return [[t for _, t in sorted(row)] for _, row in sorted(rows.items())]

    def domain(strip):
        strip.update()
        strip.repaint()
        return strip.property("domainLow"), strip.property("domainHigh")

    def zoom_in(strip):
        pos = QPointF(strip.width() * 0.5, strip.height() * 0.5)
        glob = QPointF(strip.mapToGlobal(pos.toPoint()))
        QApplication.sendEvent(strip, QWheelEvent(pos, glob, QPoint(0, 0), QPoint(0, 480), Qt.NoButton, Qt.NoModifier, Qt.NoScrollPhase, False))
        QApplication.processEvents()

    def near(a, b, tol=1e-4):
        return abs(a - b) < tol

    def hue_near(a, b, tol=1e-4):
        d = (a - b) % 1.0
        return min(d, 1.0 - d) < tol

    def steps():
        gt.section("hsvkeyer")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        gt.close_all_panels()
        node = app.createNode(PLUGIN_ID)
        require(node is not None, "create HSVKeyer", gt.plugin_problem(PLUGIN_ID))
        node.setPosition(0, 0)
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open HSVKeyer's settings panel")
        yield ("sleep", 800)
        enlarge(panel_of(node))
        yield ("sleep", 300)

        source = node.getParam("sourceColor")
        model = node.getParam("colorModel")
        hue = node.getParam("hueRange")
        sat = node.getParam("saturationRange")
        luma = node.getParam("lumaRange")
        use_hue = node.getParam("useHue")
        smooth = [node.getParam(k) for k in SMOOTH_KNOBS]
        require(None not in (source, model, hue, sat, luma, use_hue) and None not in smooth, "HSVKeyer has its source, model, range, toggle and smooth knobs")
        check(model.getValue() == 0, "the model defaults to HSV")
        check(near(centre(hue), 0.4) and near(hue.getValue(0), 0.05) and near(hue.getValue(4), 0.05), "the hue range's fields are outer low, inner low, centre, inner high, outer high")
        check(not any(k.getValue() for k in smooth), "the smooth knobs default off")

        ok = yield ("until", lambda: all_strips_shown(panel_of(node)), POLL_TIMEOUT_S)
        check(ok, "the panel shows a trapezoid strip for each of %s" % ", ".join(RANGE_KNOBS))
        # The panel wrapper is held while its strips are used: child wrappers die with it.
        panel = panel_of(node)
        for k in RANGE_KNOBS:
            strip = strip_of(panel, k)
            if strip is not None:
                check(strip.width() > 100 and strip.height() > 10, "the %s strip has a usable size (%dx%d)" % (k, strip.width(), strip.height()))
            buttons = [button_of(panel, prefix, k) for prefix in BUTTON_PREFIXES]
            for prefix, button in zip(BUTTON_PREFIXES, buttons):
                check(button is not None, "the %s row has its %s button" % (k, prefix.rstrip("_")))
            if None not in buttons:
                xs = [b.mapToGlobal(b.rect().topLeft()).x() for b in buttons]
                check(xs == sorted(xs), "the %s row's buttons run Smooth, Recentre, Reset Zoom (x %s)" % (k, xs))
                check(buttons[0].isCheckable() and not buttons[0].isChecked(), "the %s S button is checkable and starts unchecked" % k)

        rows = dimension_rows(panel)
        check(len(rows) == 3 and all(r == list(DIMENSION_NAMES) for r in rows), "each range's fields run %s (got %s)" % (", ".join(DIMENSION_NAMES), rows))

        strip_tops = [top_of(strip_of(panel, k)) for k in RANGE_KNOBS if strip_of(panel, k) is not None]
        for text in TOGGLE_LABELS:
            label = toggle_label(panel, text)
            check(label is not None, "the panel shows the %s toggle" % text)
            if label is not None and strip_tops:
                check(top_of(label) < min(strip_tops), "%s sits above the range strips (y %d < %d)" % (text, top_of(label), min(strip_tops)))

        screen_crop("hsvkeyer_hsv", panel_of(node))

        hue_offsets = offsets(hue)
        sat_offsets = offsets(sat)
        luma_offsets = offsets(luma)
        for i, v in enumerate(SOURCE):
            source.setValue(v, i)
        yield ("sleep", 300)
        check(hue_near(centre(hue), SOURCE_HUE), "HSV: the hue centre is the source hue (got %.4f)" % centre(hue))
        check(near(centre(sat), 1.0), "HSV: the saturation centre is 1 (got %.4f)" % centre(sat))
        check(near(centre(luma), 1.0), "HSV: the value centre is 1 (got %.4f)" % centre(luma))
        check(offsets(hue) == hue_offsets, "recentring keeps the hue offsets")
        check(offsets(sat) == sat_offsets and offsets(luma) == luma_offsets, "recentring keeps the saturation and luma offsets")

        model.setValue(1)
        yield ("sleep", 300)
        check(hue_near(centre(hue), SOURCE_HUE), "HSL: the hue centre stays on the source hue")
        check(near(centre(luma), 0.5), "HSL: the lightness centre is 0.5 (got %.4f)" % centre(luma))
        check(all_strips_shown(panel_of(node)), "HSL: the three strips are still shown")

        model.setValue(2)
        yield ("sleep", 300)
        check(0.0 < centre(luma) < 1.0, "OkLCh: the lightness centre is inside 0..1 (got %.4f)" % centre(luma))
        check(0.0 < centre(sat) < 0.4, "OkLCh: the chroma centre is inside 0..0.4 (got %.4f)" % centre(sat))
        screen_crop("hsvkeyer_oklch", panel_of(node))

        # Reset Zoom: 0 to 1 with every point inside, wider with one outside.
        set_range(sat, (0.1, 0.1, 0.5, 0.1, 0.1))
        set_range(luma, (0.1, 0.1, 0.5, 0.1, 0.1))
        yield ("sleep", 300)
        panel = panel_of(node)
        for k in ("saturationRange", "lumaRange"):
            strip = strip_of(panel, k)
            reset = button_of(panel, RESET_ZOOM_PREFIX, k)
            if strip is None or reset is None:
                check(False, "the %s strip and its Reset Zoom button are on screen" % k)
                continue
            check(domain(strip) == (0.0, 1.0), "%s: the initial view is 0..1 (got %s)" % (k, domain(strip)))
            zoom_in(strip)
            low, high = domain(strip)
            check(high - low < 0.9, "%s: the wheel zooms in (%.3f..%.3f)" % (k, low, high))
            reset.click()
            QApplication.processEvents()
            low, high = domain(strip)
            check(near(low, 0.0) and near(high, 1.0), "%s: Reset Zoom shows 0..1 (got %.3f..%.3f)" % (k, low, high))
        set_range(luma, (0.1, 0.1, 0.5, 0.1, 0.9))
        yield ("sleep", 300)
        panel = panel_of(node)
        strip = strip_of(panel, "lumaRange")
        reset = button_of(panel, RESET_ZOOM_PREFIX, "lumaRange")
        if strip is not None and reset is not None:
            zoom_in(strip)
            reset.click()
            QApplication.processEvents()
            low, high = domain(strip)
            check(near(low, 0.0) and 1.5 < high < 1.7, "luma: Reset Zoom takes in the outer high point at 1.5 with a margin (got %.3f..%.3f)" % (low, high))

        # The S button toggles the smooth knob, and the strip redraws.
        strip = strip_of(panel, "hueRange")
        button = button_of(panel, SMOOTH_PREFIX, "hueRange")
        if strip is not None and button is not None:
            before_image = strip.grab().toImage()
            button.click()
            yield ("sleep", 300)
            panel = panel_of(node)
            strip = strip_of(panel, "hueRange")
            button = button_of(panel, SMOOTH_PREFIX, "hueRange")
            check(smooth[0].getValue(), "clicking the hue S button turns hueSmooth on")
            check(button.isChecked(), "the hue S button shows checked")
            check(strip.grab().toImage() != before_image, "the hue strip redraws with smoothed falloffs")
            button.click()
            yield ("sleep", 300)
            check(not smooth[0].getValue(), "a second click turns hueSmooth off")
        else:
            check(False, "the hue strip and its S button are on screen")
        for k in smooth:
            k.setValue(True)
        yield ("sleep", 300)
        panel = panel_of(node)
        check(all(button_of(panel, SMOOTH_PREFIX, k).isChecked() for k in RANGE_KNOBS), "the S buttons follow their knobs")
        screen_crop("hsvkeyer_smooth", panel_of(node))
        for k in smooth:
            k.setValue(False)
        yield ("sleep", 300)

        panel = panel_of(node)
        before = [hue.getValue(i) for i in range(5)]
        check(strip_of(panel, "hueRange").isEnabled(), "the hue strip is enabled with the hue term on")
        use_hue.setValue(False)
        yield ("sleep", 300)
        panel = panel_of(node)
        check([hue.getValue(i) for i in range(5)] == before, "turning the hue term off leaves the hue range alone")
        hue_strip = strip_of(panel, "hueRange")
        check(hue_strip is not None, "the hue strip stays on screen with the hue term off")
        if hue_strip is not None:
            check(not hue_strip.isEnabled(), "the hue row greys out with the hue term off")
            check(not button_of(panel, RECENTRE_PREFIX, "hueRange").isEnabled(), "the hue row's Recentre button greys out too")
            check(not button_of(panel, SMOOTH_PREFIX, "hueRange").isEnabled(), "the hue row's S button greys out too")
        check(strip_of(panel, "saturationRange").isEnabled(), "the saturation row stays enabled")
        screen_crop("hsvkeyer_hue_off", panel_of(node))
        use_hue.setValue(True)
        yield ("sleep", 300)
        panel = panel_of(node)
        check(strip_of(panel, "hueRange").isEnabled(), "the hue row is enabled again with the hue term back on")

        narrow = strip_of(panel, "hueRange").width()
        how = widen(panel, 300)
        yield ("sleep", 500)
        panel = panel_of(node)
        wide = strip_of(panel, "hueRange").width()
        check(wide > narrow + 100, "the hue strip grows with the panel (%d to %d px, widened by %s)" % (narrow, wide, how))
        screen_crop("hsvkeyer_wide", panel_of(node))

        node.destroy()
        yield ("sleep", 200)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
