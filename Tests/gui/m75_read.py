"""The native Read in the GUI: layers in the viewer's layer menu, formats the old readers covered,
no ReadOIIO / ReadPNG left in the node search, and the create-time file dialog's filter.

A DPX and a TGA are written at run time with oiiotool (or OpenImageIO's Python module if that is
missing) under $NATRON_GUI_TEST_OUT/fixtures, so the container only needs one of the two.

Screenshots (cropped): m75-read-panel.png and m75-read-panel-context.png (the Read's properties panel),
m75-input-colorspace-menu.png, m75-colorspace-changed.png and m75-colorspace-reset.png (the Input Colorspace menu and its reset), m75-layer-menu.png (the
viewer's layer menu open on the multilayer EXR) and m75-dpx-viewer.png (the DPX in the viewer).

Usage: Tests/gui/run-gui-test.sh Tests/gui/m75_read.py
"""
import os
import subprocess
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 400
STARTUP_TIMEOUT_S = 60.0
READ_ID = "fr.inria.built-in.Read"

try:
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, QRect, Qt, QTimer, Q_ARG
    from PySide6.QtGui import QEnterEvent, QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QListView, QWidget

    import guitest as gt
    from guitest import check, report, require

    FIXTURE_DIR = os.path.join(gt.OUT, "fixtures")
    SOLIDS = {"dpx": ((0.1, 0.8, 0.1), "green"), "tga": ((0.1, 0.1, 0.8), "blue")}

    def make_solid(path, rgb):
        """A 64x48 solid-colour image; the format comes from the extension."""
        os.makedirs(FIXTURE_DIR, exist_ok=True)
        color = ",".join(str(v) for v in rgb)
        try:
            subprocess.run(["oiiotool", "--pattern", "constant:color=" + color, "64x48", "3", "-d", "uint8", "-o", path], check=True, capture_output=True)
            return True
        except (OSError, subprocess.CalledProcessError):
            pass
        try:
            import OpenImageIO as oiio
            import numpy
            pixels = numpy.empty((48, 64, 3), dtype=numpy.float32)
            pixels[:, :] = rgb
            out = oiio.ImageOutput.create(path)
            spec = oiio.ImageSpec(64, 48, 3, "uint8")
            ok = out.open(path, spec) and out.write_image(pixels)
            out.close()
            return bool(ok)
        except Exception:
            return False

    def dominant(colour):
        """'red', 'green' or 'blue' when one channel leads the others by a clear margin. The viewer's
        display transform tones values down, so a threshold on the channel itself would not hold."""
        if colour is None:
            return None
        order = sorted(range(3), key=lambda i: -colour[i])
        if colour[order[0]] - colour[order[1]] < 60:
            return None
        return ("red", "green", "blue")[order[0]]

    def same_colour(a, b):
        """The display transform dithers by a code value, so equal images differ by one."""
        return a is not None and b is not None and all(abs(x - y) <= 2 for x, y in zip(a, b))

    def viewer_shows(name):
        return dominant(gt.viewer_centre_colour()) == name

    def viewer_layer_combo():
        for w in gt.widgets_of("ComboBox"):
            try:
                if w.isVisible() and "Layer:" in (w.toolTip() or ""):
                    return w
            except RuntimeError:
                continue
        return None

    def combo_items(combo):
        try:
            return [str(i) for i in (combo.property("natronItems") or [])]
        except RuntimeError:
            return []

    def combo_current(combo):
        try:
            return str(combo.property("natronCurrentText") or "")
        except RuntimeError:
            return ""

    def choose(combo, text):
        items = combo_items(combo)
        index = next((n for n, i in enumerate(items) if text in i), -1)
        if index < 0:
            return False
        QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
        return True

    def screen_crop(name, rect):
        """Saves the part of the screen inside rect, clamped to the screen."""
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = rect.intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(gt.OUT, name)
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def global_rect(w):
        return QRect(w.mapToGlobal(w.rect().topLeft()), w.size())

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

    def open_panel(node):
        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open %s's settings panel" % node.getLabel())

    def click(widget):
        """A left press sent straight to widget: synthesized events through the window system do
        not reach Natron's widgets under Xvfb."""
        centre = widget.rect().center()
        event = QMouseEvent(QEvent.MouseButtonPress, QPointF(centre), QPointF(widget.mapToGlobal(centre)),
                            Qt.LeftButton, Qt.LeftButton, Qt.NoModifier)
        QApplication.sendEvent(widget, event)

    def settle_popup_then(action):
        """Runs action once the menu or dialog that the click in progress opened has been drawn."""
        QTimer.singleShot(1500, action)

    viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer node exists")
    viewer.setPosition(400, 600)
    made = []

    def connect_reader(path, x, label):
        require(os.path.isfile(path), "fixture exists: " + os.path.basename(path))
        node = app.createReader(path)
        require(node is not None, "create a Read for " + os.path.basename(path))
        require(node.getPluginID() == READ_ID, "%s is a native Read (%s)" % (label, node.getPluginID()))
        node.setPosition(x, 100)
        made.append(node)
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connect the Viewer to " + label)
        return node

    def read_is_clean(node):
        return not (node.getPersistentMessage() or "")

    def layers_section():
        gt.section("multilayer EXR")
        read = connect_reader(os.path.join(gt.FIXTURES, "flat-three-layers.exr"), 0, "the three-layer Read")
        check(read.getParam("layer") is None, "the Read has no layer knob of its own")
        ok = yield ("until", lambda: viewer_layer_combo() is not None, POLL_TIMEOUT_S)
        require(ok, "the viewer's layer menu is on screen")
        combo = viewer_layer_combo()

        def lists_all():
            items = combo_items(combo)
            return all(any(l in i for i in items) for l in ("rgba", "diffuse", "specular"))

        ok = yield ("until", lists_all, POLL_TIMEOUT_S)
        check(ok, "the layer menu lists rgba, diffuse and specular (%r)" % combo_items(combo))
        ok = yield ("until", lambda: viewer_shows("red"), POLL_TIMEOUT_S)
        check(ok, "the default layer displays red (%r)" % (gt.viewer_centre_colour(),))
        check(read_is_clean(read), "the Read reports no error (%r)" % read.getPersistentMessage())

        shots = {"done": False}

        def grab_menu():
            popup = QApplication.activePopupWidget()
            rect = global_rect(combo)
            if popup is not None:
                rect = rect.united(global_rect(popup))
            screen_crop("m75-layer-menu.png", rect.adjusted(-20, -20, 20, 20))
            shots["popup"] = popup is not None
            if popup is not None:
                QTest.keyClick(popup, Qt.Key_Escape)
            shots["done"] = True

        settle_popup_then(grab_menu)
        click(combo)
        check(shots.get("popup", False), "the layer menu opened as a popup")

        for name, colour in (("diffuse", "green"), ("specular", "blue"), ("rgba", "red")):
            require(choose(combo, name), "the layer menu has a %s entry" % name)
            ok = yield ("until", lambda c=colour: viewer_shows(c), POLL_TIMEOUT_S)
            check(ok, "choosing %s displays %s (%r)" % (name, colour, gt.viewer_centre_colour()))

    def knob_combo(panel, row_y):
        """The knob menu on the panel row whose label sits at row_y."""
        for w in gt.widgets_of("ComboBox"):
            try:
                if w.isVisible() and panel.isAncestorOf(w) and abs(w.mapTo(panel, w.rect().topLeft()).y() - row_y) < 16:
                    return w
            except RuntimeError:
                continue
        return None

    def panel_rows(panel):
        """(label text, y in the panel) for every visible label, top to bottom."""
        rows = []
        for w in panel.findChildren(QLabel):
            try:
                if w.isVisible() and w.text().strip():
                    rows.append((w.text().strip(), w.mapTo(panel, w.rect().topLeft()).y()))
            except RuntimeError:
                continue
        return sorted(rows, key=lambda r: r[1])

    def first_y(rows, text):
        return next((y for t, y in rows if t.rstrip(":") == text), None)

    def popup_actions(popup):
        return [(a.text(), a.menu() is not None) for a in popup.actions() if a.text()]

    def reset_through_right_click(combo):
        """Opens the knob's right-click menu the way a right click does and triggers its Reset to default."""
        seen = {}

        def trigger():
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            seen["entries"] = [a.text() for a in popup.actions() if a.text()]
            action = next((a for a in popup.actions() if a.text() == "Reset to default"), None)
            if action is not None and action.isEnabled():
                action.trigger()
                seen["triggered"] = True
            popup.close()

        QTimer.singleShot(800, trigger)
        combo.customContextMenuRequested.emit(combo.rect().center())
        return seen

    def panel_section():
        gt.section("Read panel")
        read = connect_reader(os.path.join(gt.FIXTURES, "flat-rgba-only.exr"), 0, "the RGBA Read")
        check(read.getParam("frameRate") is None, "there is no frameRate knob")
        check(read.getParam("customFps") is None, "there is no customFps knob")
        yield from open_panel(read)
        yield ("sleep", 800)
        panel = panel_of(read)
        require(panel is not None, "the Read's panel is open")
        rows = panel_rows(panel)
        report("NOTE panel labels top to bottom: %r" % rows)
        labels = [t for t, _ in rows]
        check(not any("rame rate" in l or "FPS" in l for l in labels), "no frame rate or custom FPS label on the panel")
        order = ["File", "Proxy File", "First Frame", "Last Frame", "Input Colorspace", "OCIO Context"]
        ys = [first_y(rows, n) for n in order]
        check(all(y is not None for y in ys) and ys == sorted(ys), "File, Proxy File, frame range, Input Colorspace, OCIO Context run top to bottom (%r)" % list(zip(order, ys)))
        sliders = [w for w in panel.findChildren(QWidget) if gt.class_name(w) == "ScaleSliderQt" and w.isVisible()]
        fy, ly = first_y(rows, "First Frame"), first_y(rows, "Last Frame")
        near = [w for w in sliders if fy is not None and ly is not None and fy - 10 <= w.mapTo(panel, w.rect().topLeft()).y() <= ly + 30]
        check(not near, "First Frame and Last Frame have no slider (%d slider(s) in the panel)" % len(sliders))
        screen_crop("m75-read-panel.png", global_rect(panel))

        check(read.getParam("Context") is not None, "the OCIO Context group exists")
        toggles = [w for w in panel.findChildren(QLabel) if gt.class_name(w) == "GroupBoxLabel" and w.isVisible()]
        require(len(toggles) == 1, "the panel has one group toggle (%d)" % len(toggles))
        click(toggles[0])
        yield ("sleep", 800)
        rows = panel_rows(panel)
        report("NOTE labels with the Context group open: %r" % rows)
        names = [t.rstrip(":") for t, _ in rows]
        check(all(("key%d" % n) in names and ("value%d" % n) in names for n in range(1, 5)), "key1..4 and value1..4 are shown in the Context group")
        screen_crop("m75-read-panel-context.png", global_rect(panel))

        combo = knob_combo(panel, first_y(rows, "Input Colorspace"))
        require(combo is not None, "the Input Colorspace menu is on screen")
        items = combo_items(combo)
        report("NOTE Input Colorspace: %d entries, current %r, first entries %r" % (len(items), combo_current(combo), items[:12]))

        menu = {}

        def grab_menu():
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            top = popup_actions(popup)
            menu["top"] = top
            sub = next(((a, a.menu()) for a in popup.actions() if a.menu() is not None and a.text().startswith("Input")), None)
            if sub is None:
                sub = next(((a, a.menu()) for a in popup.actions() if a.menu() is not None), None)
            if sub is not None:
                popup.setActiveAction(sub[0])
                QTest.keyClick(popup, Qt.Key_Right)
                QApplication.processEvents()
                menu["sub_name"] = sub[0].text()
                menu["sub"] = popup_actions(sub[1])
                menu["sub_visible"] = sub[1].isVisible()
            QTimer.singleShot(600, finish_menu_shot)

        def finish_menu_shot():
            popup = QApplication.activePopupWidget()
            rect = global_rect(combo)
            for w in QApplication.topLevelWidgets():
                try:
                    if w.isVisible() and gt.class_name(w) == "QMenu":
                        rect = rect.united(global_rect(w))
                except RuntimeError:
                    continue
            screen_crop("m75-input-colorspace-menu.png", rect.adjusted(-10, -10, 10, 10))
            while QApplication.activePopupWidget() is not None:
                QApplication.activePopupWidget().close()

        before_menu = combo_current(combo)
        QTimer.singleShot(1200, grab_menu)
        click(combo)
        QApplication.processEvents()
        check(bool(menu.get("top")), "the Input Colorspace menu opened as a popup")
        nested = [n for n, is_menu in menu.get("top", []) if is_menu]
        report("NOTE top-level menu entries: %r" % menu.get("top"))
        check(len(nested) >= 3, "the top level holds nested submenus (%r)" % nested)
        check(any("ACES" in n for n in nested) and any("Input" in n for n in nested) and any("Utility" in n for n in nested),
              "submenus include ACES, Input and Utility families")
        report("NOTE expanded submenu %r: %r (visible=%r)" % (menu.get("sub_name"), menu.get("sub"), menu.get("sub_visible")))
        check(bool(menu.get("sub_visible")), "a submenu expanded")
        check(combo_current(combo) == before_menu, "closing the menu without a choice leaves the colourspace alone")

        yield ("sleep", 800)
        viewers = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        require(bool(viewers), "the viewer is on screen")
        covered = global_rect(panel).intersects(global_rect(viewers[0]))
        if covered:
            panel.window().move(panel.window().x() + 700, panel.window().y())
            yield ("sleep", 500)
        yield ("sleep", 1500)
        default_text = combo_current(combo)
        default_colour = gt.viewer_centre_colour()
        report("NOTE default colourspace %r displays %r" % (default_text, default_colour))
        alt = next((i for i in items if "sRGB" in i and i != default_text), None)
        require(alt is not None, "an sRGB colourspace is on the menu")
        require(choose(combo, alt), "pick %s in the menu" % alt)
        ok = yield ("until", lambda: gt.viewer_centre_colour() != default_colour, POLL_TIMEOUT_S)
        changed = gt.viewer_centre_colour()
        check(ok, "choosing %s changes the viewer image (%r -> %r)" % (alt, default_colour, changed))
        check(combo_current(combo) != default_text, "the menu shows the new choice (%r)" % combo_current(combo))
        yield ("sleep", 800)
        screen_crop("m75-colorspace-changed.png", global_rect(panel))

        seen = reset_through_right_click(combo)
        check(seen.get("triggered", False), "the right-click menu offers an enabled Reset to default (%r)" % seen.get("entries"))
        ok = yield ("until", lambda: same_colour(gt.viewer_centre_colour(), default_colour), POLL_TIMEOUT_S)
        check(ok, "Reset to default restores the viewer image (%r -> %r, default %r)" % (changed, gt.viewer_centre_colour(), default_colour))
        check(combo_current(combo) == default_text, "the menu is back on %r (shows %r)" % (default_text, combo_current(combo)))
        yield ("sleep", 800)
        screen_crop("m75-colorspace-reset.png", global_rect(panel))
        gt.close_all_panels()

    def format_section():
        gt.section("DPX and TGA")
        for ext, x in (("dpx", 200), ("tga", 400)):
            path = os.path.join(FIXTURE_DIR, "m75-solid." + ext)
            rgb, expected = SOLIDS[ext]
            require(make_solid(path, rgb), "write a run-time %s" % ext.upper(), "no oiiotool or OpenImageIO")
            read = connect_reader(path, x, "the %s Read" % ext.upper())
            ok = yield ("until", lambda: viewer_shows(expected), POLL_TIMEOUT_S)
            check(ok, "the %s displays %s through the native Read (%r)" % (ext.upper(), expected, gt.viewer_centre_colour()))
            yield ("sleep", SETTLE_MS)
            check(read_is_clean(read), "the %s Read reports no error (%r)" % (ext.upper(), read.getPersistentMessage()))
            if ext == "dpx":
                viewers = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
                if viewers:
                    screen_crop("m75-dpx-viewer.png", global_rect(viewers[0]))

    def open_node_dialog():
        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        # The Tab shortcut only works while the pointer is over the graph, which the graph learns
        # from its enter event; Xvfb has no pointer to move there.
        centre = QPointF(view.rect().center())
        QApplication.sendEvent(view, QEnterEvent(centre, centre, QPointF(view.mapToGlobal(view.rect().center()))))
        QTest.keyClick(view, Qt.Key_Tab)
        ok = yield ("until", lambda: any(d.isVisible() for d in gt.widgets_of("NodeCreationDialog")), POLL_TIMEOUT_S)
        require(ok, "the Tab menu opens")
        dialog = next(d for d in gt.widgets_of("NodeCreationDialog") if d.isVisible())
        return dialog, dialog.findChildren(QLineEdit)[0], dialog.findChildren(QListView)[0]

    def listed(line_edit, view, text):
        line_edit.setText(text)
        QApplication.processEvents()
        model = view.model()
        return [str(model.index(r, 0).data()) for r in range(model.rowCount())]

    def search_section():
        gt.section("node search")
        dialog, line_edit, view = yield from open_node_dialog()
        entries = listed(line_edit, view, "Read")
        report("NOTE search 'Read': %r" % entries)
        check(any(e.split("[")[0].strip() == "Read" for e in entries), "searching Read finds the Read node")
        for name in ("ReadOIIO", "ReadPNG"):
            found = listed(line_edit, view, name)
            report("NOTE search %r: %r" % (name, found))
            check(not any(name.lower() in e.lower() for e in found), "searching %s finds no such node" % name)
        QTest.keyClick(line_edit, Qt.Key_Escape)
        QApplication.processEvents()

    def dialog_section():
        gt.section("create-time file dialog")
        seen = {}

        def inspect():
            dialogs = [d for d in gt.widgets_of("SequenceFileDialog") if d.isVisible()]
            if not dialogs:
                QTimer.singleShot(200, inspect)
                return
            dialog = dialogs[0]

            def read_menu():
                popup = QApplication.activePopupWidget()
                if popup is None:
                    return
                seen["items"] = [a.text() for a in popup.actions()]
                screen_crop("m75-file-dialog-filter.png",
                            global_rect(dialog).united(global_rect(popup)).adjusted(-10, -10, 10, 10))
                QTest.keyClick(popup, Qt.Key_Escape)

            # The open dialog shows its extension list only in the menu behind the filter's drop-down.
            QTimer.singleShot(800, read_menu)
            QMetaObject.invokeMethod(dialog, "showFilterMenu")
            seen["dialog"] = True
            dialog.reject()

        dialog, line_edit, view = yield from open_node_dialog()
        entries = listed(line_edit, view, "Read")
        row = next((r for r, e in enumerate(entries) if e.split("[")[0].strip() == "Read"), -1)
        require(row >= 0, "the Tab menu offers Read")
        view.setCurrentIndex(view.model().index(row, 0))
        QTimer.singleShot(500, inspect)
        # Return creates the Read, whose creation opens the modal file dialog; the call returns once
        # inspect() has rejected it.
        QTest.keyClick(line_edit, Qt.Key_Return)
        QApplication.processEvents()
        check(seen.get("dialog", False), "the create-time file dialog opened")
        items = [i.lower() for i in seen.get("items", [])]
        check(len(items) > 3, "the dialog's filter menu opened (%d entries)" % len(items))
        check("*.dpx" in items, "the dialog's filter lists dpx")
        check("*.tga" in items, "the dialog's filter lists tga")
        check("*.cr2" not in items, "the dialog's filter does not list cr2")
        check(not any(i in items for i in ("*.raw", "*.psd", "*.ffmpeg")), "the dialog's filter lists no raw, psd or ffmpeg")

    def steps():
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "viewer widget is up")
        gt.close_all_panels()
        yield from layers_section()
        yield from panel_section()
        yield from format_section()
        yield from search_section()
        yield from dialog_section()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
