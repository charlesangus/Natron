"""Screenshots and checks for node label wrapping, take-over picker labels and alignment, themed
icons, and layout density.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m56-shots/out -e GUI_TEST_TIMEOUT=300 \
        -w /home/bosley/git/Natron natron-dev \
        Tests/gui/run-gui-test.sh Tests/gui/m56_visual.py build/fast/App/Natron
Run it again with -e QT_SCALE_FACTOR=2 for the hi-DPI shots. Set M56_BASELINE_DIR to the output
directory of an earlier run to also check that the themed icons differ from that run's icons.

Sizes are compared in logical px scaled by the screen's logical DPI / 96, the same factor
TO_DPIX/TO_DPIY apply.

Knob widgets carry no object name, but KnobGui puts the knob's script name in bold at the start
of the tooltip it sets on both the label and the field, so that is how a knob's widgets are found.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 500
PANEL_MS = 900

NODE_WIDTH = 80
NODE_MAX_WIDTH = NODE_WIDTH * 2.5
ALIGN_TOLERANCE = 2

ROW_GAP = 4
GROUP_INDENT = 12
PANEL_MARGIN = 6
PANE_TOP_MARGIN = 2
TAB_HEIGHT = 26
MENU_TOP_PADDING = 4

THEMED_COLOUR_TOLERANCE = 12
THEMED_PIXEL_FRACTION = 0.9
THEMED_ICON_FRACTION = 0.8
ICON_PROBE_SIZE = 24

try:
    import NatronEngine
    import shiboken6
    from PySide6.QtCore import QBuffer, QByteArray, QEvent, QIODevice, QMetaObject, QPoint, QPointF, QRect, QRectF, QSize, Qt, QTimer
    from PySide6.QtGui import QGuiApplication, QIcon, QMouseEvent
    from PySide6.QtWidgets import (QAbstractButton, QApplication, QGraphicsTextItem, QLabel, QMenu, QScrollArea,
                                   QTabBar, QToolBar, QWidget)

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _icon_hashes = {}

    def dpi_scale():
        return QGuiApplication.primaryScreen().logicalDotsPerInchX() / 96.0

    def px(logical):
        return logical * dpi_scale()

    def near(value, logical, tol=0.5):
        return abs(value - px(logical)) <= tol

    def ptr(obj):
        return shiboken6.getCppPointer(obj)[0]

    def record_binary():
        binary = QApplication.applicationFilePath()
        try:
            mtime = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(binary)))
        except OSError:
            mtime = "?"
        try:
            describe = subprocess.run(["git", "-C", gt.ROOT, "describe", "--always", "--dirty", "--tags"],
                                      capture_output=True, text=True, timeout=20).stdout.strip()
        except Exception:
            describe = "?"
        report("NOTE binary %s (built %s); git describe of the tree it was run from: %s"
               % (binary, mtime, describe or "?"))
        report("NOTE QT_SCALE_FACTOR=%s, logical DPI scale %.3f, device pixel ratio %.2f"
               % (os.environ.get("QT_SCALE_FACTOR", "unset"), dpi_scale(), QApplication.instance().devicePixelRatio()))

    # ---- nodes, panes and widgets --------------------------------------------------------------

    def create(plugin_id, x, y, label=None):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        if label is not None:
            node.setLabel(label)
        return node

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def gui_widget():
        found = gt.widgets_of("Gui")
        return found[0] if found else None

    def visible_panels():
        return [w for w in gt.widgets_of("NodeSettingsPanel") if w.isVisible()]

    def panel_for(node):
        """The visible settings panel whose name field shows node's label."""
        for p in visible_panels():
            for le in p.findChildren(QWidget):
                try:
                    if hasattr(le, "text") and gt.class_name(le) == "LineEdit" and le.text() == node.getLabel():
                        return p
                except (RuntimeError, TypeError):
                    continue
        panels = visible_panels()
        return panels[0] if len(panels) == 1 else None

    def descendants(root, cls=QWidget):
        try:
            return [w for w in root.findChildren(cls) if w.isVisible()]
        except RuntimeError:
            return []

    def is_inside(widget, ancestor_ptrs):
        w = widget
        while w is not None:
            if ptr(w) in ancestor_ptrs:
                return True
            w = w.parentWidget()
        return False

    def pane(name):
        for w in gt.widgets_of("TabWidget"):
            if w.objectName() == name:
                return w
        return None

    def make_tab_current(pane_name, label_substr):
        tw = app.getTabWidget(pane_name)
        if tw is None:
            return False
        for i in range(tw.count()):
            if label_substr.lower() in tw.getTabLabel(i).lower():
                tw.setCurrentIndex(i)
                QApplication.processEvents()
                return True
        return False

    def global_rect(widget):
        tl = widget.mapToGlobal(QPoint(0, 0))
        return tl.x(), tl.y(), widget.width(), widget.height()

    def crop_shot(widget, name, pad=0):
        """A screen crop rather than widget.grab() where possible, so GL children (viewer,
        timeline, dope sheet) come out drawn."""
        if widget is None or not widget.isVisible():
            report("NOTE %s: nothing visible to shoot" % name)
            return
        QApplication.processEvents()
        x, y, w, h = global_rect(widget)
        screen = QGuiApplication.primaryScreen()
        if screen.geometry().contains(QRect(x - pad, y - pad, w + 2 * pad, h + 2 * pad)):
            pix = screen.grabWindow(0, x - pad, y - pad, w + 2 * pad, h + 2 * pad)
        else:
            pix = widget.grab()
            report("NOTE %s: partly off screen, grabbed from the widget instead" % name)
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def open_menus():
        menus = []
        popup = QApplication.activePopupWidget()
        if isinstance(popup, QMenu):
            menus.append(popup)
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMenu) and w.isVisible() and all(ptr(w) != ptr(m) for m in menus):
                menus.append(w)
        return menus

    def close_popups():
        for _ in range(4):
            menus = open_menus()
            if not menus:
                return
            for m in reversed(menus):
                m.close()
            QApplication.processEvents()

    def right_click(widget, pos, on_menu, settle_ms=500):
        """QMenu::exec runs a nested event loop inside the press handler, so on_menu is scheduled
        before the press and runs inside that loop; it must close the menu when done. A watchdog
        keeps closing menus until the press returns, so a missed menu cannot hang the run."""
        done = [False]

        def fire():
            menus = open_menus()
            try:
                on_menu(menus[0] if menus else None)
            except Exception:
                check(False, "menu handler error: " + traceback.format_exc())
                close_popups()

        def guard():
            if done[0]:
                return
            close_popups()
            QTimer.singleShot(2000, guard)
        QTimer.singleShot(settle_ms, fire)
        QTimer.singleShot(settle_ms + 4000, guard)
        ev = QMouseEvent(QEvent.MouseButtonPress, QPointF(pos), widget.mapToGlobal(QPointF(pos)),
                         Qt.RightButton, Qt.RightButton, Qt.NoModifier)
        QApplication.sendEvent(widget, ev)
        done[0] = True
        QApplication.processEvents()

    # ---- themed-icon probe ---------------------------------------------------------------------

    def theme_colours():
        settings = NatronEngine.natron.getSettings()
        colours = []
        for name in ("text", "selection"):
            param = settings.getParam(name) if settings is not None else None
            if param is not None:
                colours.append(tuple(int(round(param.getValue(i) * 255)) for i in range(3)))
        return colours or [(199, 199, 199)]

    def icon_image(icon):
        pix = icon.pixmap(QSize(ICON_PROBE_SIZE, ICON_PROBE_SIZE), QIcon.Normal, QIcon.Off)
        return pix, pix.toImage()

    def themed_fraction(img, colours):
        opaque = 0
        themed = 0
        for y in range(img.height()):
            for x in range(img.width()):
                c = img.pixelColor(x, y)
                if c.alpha() < 160:
                    continue
                opaque += 1
                for r, g, b in colours:
                    if max(abs(c.red() - r), abs(c.green() - g), abs(c.blue() - b)) <= THEMED_COLOUR_TOLERANCE:
                        themed += 1
                        break
        return (themed / float(opaque)) if opaque else None, opaque

    def image_hash(img):
        data = QByteArray()
        buf = QBuffer(data)
        buf.open(QIODevice.WriteOnly)
        img.save(buf, "PNG")
        buf.close()
        return hashlib.md5(bytes(data)).hexdigest()

    def describe_icon_owner(owner):
        text = ""
        for getter in ("text", "toolTip"):
            try:
                text = getattr(owner, getter)() or ""
            except Exception:
                text = ""
            if text:
                break
        return re.sub(r"<[^>]*>", " ", text).replace("\n", " ").strip()[:40] or gt.class_name(owner)

    def icon_entries(owners):
        """(description, icon copy) for each button or action that has an icon."""
        entries = []
        for o in owners:
            try:
                ic = o.icon()
            except RuntimeError:
                continue
            if ic is not None and not ic.isNull():
                entries.append((describe_icon_owner(o), QIcon(ic)))
        return entries

    def check_icons(area, owners):
        """owners are buttons or actions. Every icon must be non-null, and most must be drawn in
        the theme's text or selection colour: the PNG icons were not."""
        check_icon_entries(area, icon_entries(owners))

    def check_icon_entries(area, icons):
        colours = theme_colours()
        if not check(len(icons) > 0, "%s: found icons to probe (%d)" % (area, len(icons))):
            return
        nulls = 0
        themed = 0
        probed = 0
        off = []
        hashes = []
        for owner, ic in icons:
            pix, img = icon_image(ic)
            if pix.isNull():
                nulls += 1
                continue
            hashes.append(image_hash(img))
            frac, opaque = themed_fraction(img, colours)
            if frac is None:
                continue
            probed += 1
            if frac >= THEMED_PIXEL_FRACTION:
                themed += 1
            else:
                off.append("%s (%.0f%%)" % (owner, frac * 100))
        check(nulls == 0, "%s: every icon pixmap is non-null (%d null of %d)" % (area, nulls, len(icons)))
        ratio = (themed / float(probed)) if probed else 0.0
        check(probed > 0 and ratio >= THEMED_ICON_FRACTION,
              "%s: %d of %d icons are drawn in the theme colour (need %.0f%%)"
              % (area, themed, probed, THEMED_ICON_FRACTION * 100))
        if off:
            report("NOTE %s: icons not in the theme colour: %s" % (area, ", ".join(off[:12])))
        _icon_hashes[area] = hashes
        compare_with_baseline(area, hashes)

    def compare_with_baseline(area, hashes):
        base_dir = os.environ.get("M56_BASELINE_DIR")
        if not base_dir:
            return
        path = os.path.join(base_dir, "icon-hashes.json")
        try:
            with open(path) as f:
                baseline = json.load(f).get(area)
        except (OSError, ValueError):
            baseline = None
        if not baseline:
            report("NOTE %s: no baseline icons in %s" % (area, path))
            return
        unchanged = len(set(hashes) & set(baseline))
        check(unchanged <= len(hashes) * (1.0 - THEMED_ICON_FRACTION),
              "%s: icons differ from the baseline run (%d of %d unchanged)" % (area, unchanged, len(hashes)))

    def save_icon_hashes():
        with open(os.path.join(gt.OUT, "icon-hashes.json"), "w") as f:
            json.dump(_icon_hashes, f, indent=1, sort_keys=True)

    def buttons_in(root, exclude_ptrs=()):
        return [b for b in descendants(root, QAbstractButton) if ptr(b) not in exclude_ptrs]

    # ---- node label geometry -------------------------------------------------------------------

    def name_rects(graph, nodes):
        """Each node's name-item scene rect, keyed by label, matched on its text. Never call
        parentItem() here: the wrapper PySide builds for the NodeGui parent deletes the node's
        graphics item when it is collected, and the next Effect.getSize() then crashes."""
        rects = {}
        for node in nodes:
            label = node.getLabel()
            x, y, w, h = node_rect(node)
            area = graph.mapFromScene(QRectF(x - 60, y - 60, w + 120, h + 120)).boundingRect()
            for it in graph.items(area):
                if not isinstance(it, QGraphicsTextItem):
                    continue
                if it.toPlainText().split("\n")[0].strip().startswith(label[:20]):
                    rects[label] = QRectF(it.sceneBoundingRect())
                    break
        return rects

    def frame_nodes(graph, nodes, max_zoom=1.5):
        xs, ys, xe, ye = [], [], [], []
        for n in nodes:
            x, y, w, h = node_rect(n)
            xs.append(x)
            ys.append(y)
            xe.append(x + w)
            ye.append(y + h)
        vp = graph.viewport()
        zoom = min(max_zoom, vp.width() / (max(xe) - min(xs) + 160.0), vp.height() / (max(ye) - min(ys) + 160.0))
        graph.resetTransform()
        graph.scale(zoom, zoom)
        graph.centerOn((max(xe) + min(xs)) / 2.0, (max(ye) + min(ys)) / 2.0)
        QApplication.processEvents()

    def node_rect(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return x, y, w, h

    def check_wrap(rects, node, case, ref_height=None):
        x, y, w, h = node_rect(node)
        r = rects.get(node.getLabel())
        report("NOTE wrap %s: node %.1fx%.1f at (%.1f, %.1f)" % (case, w, h, x, y))
        check(w <= px(NODE_MAX_WIDTH) + 0.5,
              "%s: node width %.1f <= max %.1f" % (case, w, px(NODE_MAX_WIDTH)))
        if not check(r is not None, "%s: found the node's name item" % case):
            return
        report("NOTE wrap %s: label rect (%.1f, %.1f) %.1fx%.1f" % (case, r.x(), r.y(), r.width(), r.height()))
        tol = 1.0
        inside = (r.left() >= x - tol and r.right() <= x + w + tol and
                  r.top() >= y - tol and r.bottom() <= y + h + tol)
        check(inside, "%s: label rect lies inside the node rect" % case)
        if ref_height is not None:
            check(h > ref_height + 0.5, "%s: node grew taller than a default-named one (%.1f > %.1f)"
                  % (case, h, ref_height))

    # ---- knob widgets --------------------------------------------------------------------------

    def knob_widgets(panel, knob_name):
        tag = "<b>%s</b>" % knob_name
        labels = []
        fields = []
        for w in descendants(panel):
            try:
                tt = w.toolTip()
            except RuntimeError:
                continue
            if not tt or tag not in tt[:200]:
                continue
            if isinstance(w, QLabel):
                labels.append(w)
            else:
                fields.append(w)
        field_ptrs = set(ptr(f) for f in fields)
        outer = [f for f in fields if f.parentWidget() is None or not is_inside(f.parentWidget(), field_ptrs)]
        return labels, outer

    def left_in(panel, widget):
        return widget.mapTo(panel, QPoint(0, 0)).x()

    def top_in(panel, widget):
        return widget.mapTo(panel, QPoint(0, 0)).y()

    def check_picker_label(panel, plugin, knob, want_text):
        labels, fields = knob_widgets(panel, knob)
        texts = [l.text().strip() for l in labels]
        report("NOTE %s %s: labels %r, %d field widget(s)" % (plugin, knob, texts, len(fields)))
        check(len(fields) > 0, "%s: the %s picker is shown" % (plugin, knob))
        check(any(texts), "%s: the %s picker has a non-empty label" % (plugin, knob))
        if want_text is not None:
            check(want_text in texts, "%s: the %s picker is labelled %r" % (plugin, knob, want_text))
        return labels, fields

    def label_containers(panel):
        """The per-row label cells: each knob label sits in a QHBoxLayout'd container widget."""
        cells = []
        for w in descendants(panel, QLabel):
            tt = w.toolTip() or ""
            if "<b>" not in tt[:60]:
                continue
            parent = w.parentWidget()
            if parent is not None and parent.layout() is not None and gt.class_name(parent) == "QWidget":
                cells.append(parent)
        return cells

    # ---- steps ---------------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        record_binary()
        main = gui_widget()
        if main is not None:
            main.window().setGeometry(QGuiApplication.primaryScreen().availableGeometry())
            yield ("sleep", SETTLE_MS)
            report("NOTE main window %dx%d" % (main.window().width(), main.window().height()))
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        # ---- wrap
        gt.section("wrap")
        graph = root_graph()
        reference = create("net.sf.cimg.CImgBlur", -300, 0)
        short = create("net.sf.cimg.CImgBlur", 0, 0, "Blur_abc")
        mid = create("net.sf.cimg.CImgBlur", 300, 0, "Blur with a thirty char label!")
        long_name = "Blur_" + "".join("%d" % (i % 10) for i in range(75))
        longn = create("net.sf.cimg.CImgBlur", 700, 0, long_name)
        grade = create("net.sf.openfx.GradePlugin", 0, 300, "GradeWithUserLabel")
        user_text = grade.getParam("userTextArea")
        if check(user_text is not None, "Grade has a user label (userTextArea) param"):
            user_text.set("first line of a user label\nsecond, somewhat longer line of the user label")
        icon_node = create("net.sf.openfx.IDistort", 300, 300, "IDistort with plugin icon label")
        preview = create("net.sf.cimg.CImgBlur", 700, 300, "Blur with a preview, long label")
        prev_param = preview.getParam("enablePreview")
        if check(prev_param is not None, "Blur has the enablePreview param"):
            prev_param.set(True)
        backdrop = create("fr.inria.built-in.BackDrop", 0, 700)
        dot = create("fr.inria.built-in.Dot", 600, 700)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        backdrop_size = backdrop.getSize()
        dot_size = dot.getSize()
        backdrop.setLabel("A backdrop whose label is far longer than its frame")
        dot.setLabel("A dot with a long label that must not resize it")
        yield ("sleep", SETTLE_MS)

        _rx, _ry, rw, rh = node_rect(reference)
        _sx, _sy, sw, sh = node_rect(short)
        check(abs(sw - rw) <= 1.5 and abs(sh - rh) <= 1.5,
              "8 chars: a short name keeps the size of a default-named Blur (%.1fx%.1f vs %.1fx%.1f)"
              % (sw, sh, rw, rh))
        app.clearSelection()
        frame_nodes(graph, [short, mid, longn, grade, icon_node, preview])
        yield ("sleep", SETTLE_MS)
        rects = name_rects(graph, [short, mid, longn, grade, icon_node, preview])
        check_wrap(rects, short, "8 chars")
        check_wrap(rects, mid, "30 chars")
        check_wrap(rects, longn, "80 chars, no spaces", rh)
        check_wrap(rects, grade, "Grade with a multi-line user label")
        check_wrap(rects, icon_node, "30 chars with a plugin icon")
        check_wrap(rects, preview, "30 chars with a preview")
        check(all(abs(a - b) <= 1.5 for a, b in zip(backdrop.getSize(), backdrop_size)),
              "Backdrop keeps its size with a long label (%r -> %r)" % (backdrop_size, backdrop.getSize()))
        check(all(abs(a - b) <= 1.5 for a, b in zip(dot.getSize(), dot_size)),
              "Dot keeps its size with a long label (%r -> %r)" % (dot_size, dot.getSize()))
        crop_shot(graph.viewport(), "wrap-nodes")
        for n in [reference, backdrop, dot, short, mid, longn, grade, icon_node, preview]:
            n.destroy()
        yield ("sleep", SETTLE_MS)

        # ---- labels
        gt.section("labels")
        for plugin_id, short_name, expected in (("net.sf.openfx.Premult", "Premult", "Premult by"),
                                                ("net.sf.openfx.Unpremult", "Unpremult", "Unpremult by")):
            gt.close_all_panels()
            node = create(plugin_id, 0, 0)
            yield ("sleep", PANEL_MS)
            panel = panel_for(node)
            if check(panel is not None, "%s: settings panel is open" % short_name):
                _labels, pick_fields = check_picker_label(panel, short_name, "hostUnPremultByChannel", expected)
                _l, layer_fields = knob_widgets(panel, "hostInputLayer")
                if pick_fields and layer_fields:
                    check(abs(top_in(panel, pick_fields[0]) - top_in(panel, layer_fields[0])) > 2,
                          "%s: the picker sits on its own row, not the layer row" % short_name)
                crop_shot(panel, "labels-%s" % short_name.lower())
            node.destroy()

        for plugin_id, short_name in (("net.sf.openfx.IDistort", "IDistort"), ("net.sf.openfx.STMap", "STMap")):
            gt.close_all_panels()
            node = create(plugin_id, 0, 0)
            yield ("sleep", PANEL_MS)
            panel = panel_for(node)
            twins = [p.getScriptName() for p in node.getParams() if p.getScriptName().startswith("hostChannel")]
            report("NOTE %s channel pickers: %r" % (short_name, twins))
            if check(panel is not None, "%s: settings panel is open" % short_name):
                check(len(twins) == 3, "%s: has U, V and Alpha channel pickers (%d)" % (short_name, len(twins)))
                lefts = []
                tops = []
                for knob in twins:
                    _labels, fields = check_picker_label(panel, short_name, knob, None)
                    if fields:
                        lefts.append(left_in(panel, fields[0]))
                        tops.append(top_in(panel, fields[0]))
                report("NOTE %s picker field left x %r, top y %r" % (short_name, lefts, tops))
                if check(len(lefts) == len(twins) and lefts, "%s: found every picker's field" % short_name):
                    check(max(lefts) - min(lefts) <= ALIGN_TOLERANCE,
                          "%s: U, V and Alpha fields share a left x within %d px (%r)"
                          % (short_name, ALIGN_TOLERANCE, lefts))
                    check(len(set(tops)) == len(tops),
                          "%s: U, V and Alpha each sit on their own row (%r)" % (short_name, tops))
                crop_shot(panel, "labels-%s" % short_name.lower())
            node.destroy()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        # ---- icons
        gt.section("icons")
        toolbars = [t for t in QApplication.allWidgets() if isinstance(t, QToolBar) and t.isVisible()]
        toolbox = toolbars[0] if toolbars else None
        if check(toolbox is not None, "found the left toolbox"):
            check_icons("toolbox", buttons_in(toolbox))
            crop_shot(toolbox, "icons-toolbox")

        viewer = create("fr.inria.built-in.Viewer", 0, 400)
        gt.close_all_panels()
        make_tab_current("pane1", "viewer")
        ok = yield ("until", lambda: any(w.isVisible() for w in gt.widgets_of("ViewerTab")), 15)
        viewer_tab = next((w for w in gt.widgets_of("ViewerTab") if w.isVisible()), None)
        if check(ok and viewer_tab is not None, "a viewer tab is shown"):
            yield ("sleep", SETTLE_MS)
            viewer_buttons = buttons_in(viewer_tab)
            check_icons("viewer toolbar and timeline", viewer_buttons)
            crop_shot(viewer_tab, "icons-viewer")
        viewer_ptrs = set(ptr(b) for b in (buttons_in(viewer_tab) if viewer_tab is not None else []))

        grade = create("net.sf.openfx.GradePlugin", 300, 0)
        yield ("sleep", PANEL_MS)
        panel = panel_for(grade)
        if check(panel is not None, "Grade: settings panel is open"):
            header = [b for b in buttons_in(panel)
                      if top_in(panel, b) < px(40) and gt.class_name(b) != "NodeColorButton"]
            check_icons("Grade panel header", header)
            crop_shot(panel, "icons-grade-panel")
        grade.destroy()
        gt.close_all_panels()

        for plugin_id, area in (("fr.inria.built-in.RotoPaint", "RotoPaint viewer toolbar"),
                                ("fr.inria.built-in.Tracker", "Tracker viewer toolbar")):
            gt.close_all_panels()
            node = create(plugin_id, 600, 0)
            viewer.connectInput(0, node)
            app.clearSelection()
            app.selectNode(node, True)
            yield ("sleep", PANEL_MS)
            if viewer_tab is not None:
                extra = buttons_in(viewer_tab, viewer_ptrs)
                check_icons(area, extra)
                crop_shot(viewer_tab, "icons-" + area.split()[0].lower() + "-viewer")
            node.destroy()
            yield ("sleep", SETTLE_MS)
        gt.close_all_panels()

        anim = create("net.sf.cimg.CImgBlur", 0, 200, "AnimatedBlur")
        size = anim.getParam("size")
        if check(size is not None, "Blur has a size param to animate"):
            for frame, value in ((1, 0.0), (10, 8.0), (20, 3.0), (30, 12.0)):
                size.setValueAtTime(value, frame, 0)
                size.setValueAtTime(value * 0.5, frame, 1)
        yield ("sleep", SETTLE_MS)

        def interp_menu_handler(area, shot_name, seen):
            def on_menu(menu):
                seen["menu"] = menu is not None
                if menu is None:
                    return
                interp = next((a for a in menu.actions()
                               if a.menu() is not None and "interpolation" in a.text().lower()), None)
                seen["interp"] = interp is not None
                if interp is None:
                    close_popups()
                    return
                sub = interp.menu()
                entries = [a for a in sub.actions() if not a.isSeparator()]
                seen["actions"] = [a.text() for a in entries]
                seen["icons"] = icon_entries(entries)
                geo = menu.actionGeometry(interp)
                sub.popup(menu.mapToGlobal(geo.topRight()))

                def finish_menu():
                    try:
                        gt.shot(shot_name + ".png")
                    finally:
                        close_popups()
                QTimer.singleShot(400, finish_menu)
            return on_menu

        for tab_label, view_class, area, shot_name in (
                ("dope", "DopeSheetView", "dope-sheet interpolation menu", "icons-dopesheet-menu"),
                ("curve", "CurveWidget", "curve-editor interpolation menu", "icons-curve-editor-menu")):
            make_tab_current("pane3", tab_label)
            yield ("sleep", PANEL_MS)
            view = next((w for w in gt.widgets_of(view_class) if w.isVisible()), None)
            if not check(view is not None, "%s: the %s is shown" % (area, view_class)):
                continue
            if view_class == "DopeSheetView":
                crop_shot(pane("pane3"), "icons-dopesheet-keyframes")
            else:
                crop_shot(pane("pane3"), "icons-curve-editor")
            seen = {}
            right_click(view, QPoint(view.width() // 2, view.height() // 2),
                        interp_menu_handler(area, shot_name, seen))
            yield ("sleep", SETTLE_MS)
            check(seen.get("menu", False), "%s: the right-click menu opened" % area)
            check(seen.get("interp", False), "%s: it has an Interpolation submenu" % area)
            actions = seen.get("actions", [])
            report("NOTE %s entries: %r" % (area, actions))
            if seen.get("icons") or view_class == "DopeSheetView":
                check_icon_entries(area, seen.get("icons", []))
            else:
                report("NOTE %s: its entries have no icons" % area)
            check(any(t.replace("&", "").lower().startswith("free") for t in actions),
                  "%s: has a Free entry" % area)
        make_tab_current("pane3", "node graph")
        anim.destroy()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        split = app.getTabWidget("pane1").splitHorizontally()
        yield ("sleep", SETTLE_MS)
        split_pane = pane(split.getScriptName()) if split is not None else None
        if check(split_pane is not None, "split the viewer pane"):
            QMetaObject.invokeMethod(split_pane, "moveScriptEditorHere")
            yield ("sleep", PANEL_MS)
            editor = next((w for w in gt.widgets_of("ScriptEditor") if w.isVisible()), None)
            if check(editor is not None, "the Script Editor is shown in the new pane"):
                check_icons("Script Editor toolbar", buttons_in(editor))
                editor_ptrs = set(ptr(w) for w in [editor] + editor.findChildren(QWidget))
                header_buttons = [b for b in buttons_in(split_pane) if ptr(b) not in editor_ptrs]
                check_icons("split pane header", header_buttons)
            crop_shot(split_pane, "icons-split-pane-script-editor")

        grade = create("net.sf.openfx.GradePlugin", 300, 0)
        yield ("sleep", PANEL_MS)
        scroll = next((w for w in QApplication.allWidgets()
                       if isinstance(w, QScrollArea) and w.objectName() == "Properties" and w.isVisible()), None)
        bin_widget = scroll.parentWidget() if scroll is not None else None
        if check(bin_widget is not None, "the properties bin is shown"):
            panel_ptrs = set()
            for p in visible_panels():
                panel_ptrs.update(ptr(w) for w in [p] + p.findChildren(QWidget))
            bin_buttons = [b for b in buttons_in(bin_widget) if ptr(b) not in panel_ptrs]
            check_icons("properties bin buttons", bin_buttons)
            if bin_buttons:
                crop_shot(bin_buttons[0].parentWidget(), "icons-properties-bin-buttons", pad=4)
            crop_shot(bin_widget, "icons-properties-bin")
        grade.destroy()
        gt.close_all_panels()
        save_icon_hashes()
        yield ("sleep", SETTLE_MS)

        # ---- density
        gt.section("density")
        grade = create("net.sf.openfx.GradePlugin", 0, 0)
        merge = create("net.sf.openfx.MergePlugin", 300, 0)
        yield ("sleep", PANEL_MS)
        gt.shot("density-main-window.png")
        panel = panel_for(grade)
        if check(panel is not None, "Grade: settings panel is open"):
            crop_shot(panel, "density-grade-panel")
            cells = label_containers(panel)
            bottoms = [c.layout().contentsMargins().bottom() for c in cells]
            report("NOTE Grade label cell bottom margins: %r" % sorted(set(bottoms)))
            check(bottoms and sum(1 for b in bottoms if near(b, ROW_GAP)) * 2 > len(bottoms),
                  "Grade: most knob rows use the %d px row gap" % ROW_GAP)
            margins = []
            for w in [panel] + panel.findChildren(QWidget):
                lay = w.layout()
                if lay is not None:
                    m = lay.contentsMargins()
                    margins.append((m.left(), m.top(), m.right(), m.bottom()))
            check(any(all(near(v, PANEL_MARGIN) for v in m) for m in margins),
                  "Grade: the panel body has a %d px margin on every side" % PANEL_MARGIN)
        merge_panel = panel_for(merge)
        if merge_panel is not None:
            crop_shot(merge_panel, "density-merge-panel")
        tab_pane = pane("pane3")
        if check(tab_pane is not None and tab_pane.layout() is not None, "found the node-graph pane"):
            top = tab_pane.layout().contentsMargins().top()
            check(near(top, PANE_TOP_MARGIN), "pane tab bar sits %d px below the pane top (got %d)"
                  % (PANE_TOP_MARGIN, top))
            bars = [b for b in tab_pane.findChildren(QTabBar) if b.isVisible() and b.count() > 0]
            if check(bool(bars), "found the node-graph pane's tab bar"):
                h = bars[0].tabRect(0).height()
                check(h <= px(TAB_HEIGHT) + 0.5, "pane tabs are at most %d px tall (got %d)" % (TAB_HEIGHT, h))
        grade.destroy()
        merge.destroy()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gui = gui_widget()
        if check(gui is not None, "found the main window"):
            QMetaObject.invokeMethod(gui, "showSettings")
            ok = yield ("until", lambda: any(w.isVisible() for w in gt.widgets_of("PreferencesPanel")), 10)
            prefs = next((w for w in gt.widgets_of("PreferencesPanel") if w.isVisible()), None)
            if check(ok and prefs is not None, "the Preferences window opened"):
                prefs.raise_()
                yield ("sleep", PANEL_MS)
                crop_shot(prefs, "density-preferences")
                prefs.close()
                yield ("sleep", SETTLE_MS)

        graph = root_graph()
        seen = {}

        def on_graph_menu(menu):
            seen["menu"] = menu is not None
            if menu is None:
                return
            acts = [a for a in menu.actions() if not a.isSeparator() and a.isVisible()]
            seen["top"] = menu.actionGeometry(acts[0]).top() if acts else None
            seen["count"] = len(acts)
            gt.shot("density-node-graph-menu.png")
            close_popups()

        vp = graph.viewport()
        right_click(vp, QPoint(vp.width() // 2, vp.height() // 2), on_graph_menu)
        yield ("sleep", SETTLE_MS)
        if check(seen.get("menu", False), "the node-graph right-click menu opened"):
            top = seen.get("top")
            report("NOTE node-graph menu: %r items, first item top y %r" % (seen.get("count"), top))
            check(top is not None and top >= px(MENU_TOP_PADDING) - 0.5,
                  "the node-graph menu has at least %d px above its first item (got %r)" % (MENU_TOP_PADDING, top))

        gt.close_all_panels()
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        writer = app.createWriter(os.path.join(gt.OUT, "density.####.exr"), None, props)
        if check(writer is not None, "created a Write node"):
            writer.setPosition(0, 300)
            groups = [p for p in writer.getParams() if isinstance(p, NatronEngine.GroupParam)]
            report("NOTE Write groups: %r" % [p.getScriptName() for p in groups])
            for g in groups:
                g.setOpened(True)
            yield ("sleep", PANEL_MS)
            panel = panel_for(writer)
            if check(panel is not None, "Write: settings panel is open"):
                lefts = sorted(set(c.layout().contentsMargins().left() for c in label_containers(panel)))
                report("NOTE Write label cell left margins: %r" % lefts)
                check(any(near(v, GROUP_INDENT) for v in lefts),
                      "Write: a group's child rows are indented %d px" % GROUP_INDENT)
                crop_shot(panel, "density-write-panel-group")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
