"""The native Write in the GUI: the create-time save dialog and its filter, the panel's layout,
a viewed Write writing nothing until rendered, rendering from the Render button through the render dialog and its progress dialog,
read back gated by the hash stored in the file (including setting an edit back) and the Reload button, the output colourspace menu, no WriteOIIO / WritePNG left in the
node search, and Render Selected on a non-writer node.

Fixtures are written under $NATRON_GUI_TEST_OUT/fixtures/m76 by the Write itself. The Reload
section recolours a written frame in place with oiiotool (or OpenImageIO's Python module if that
is missing).

Screenshots (cropped unless noted): m76-write-panel.png (the Write's properties panel),
m76-output-colorspace-menu.png (the Output Colorspace menu open on a submenu),
m76-colorspace-changed.png and m76-colorspace-reset.png, m76-save-dialog-filter.png (the create-time
save dialog with its filter menu) and m76-render-finished.png (the whole screen after the render).

Usage: Tests/gui/run-gui-test.sh Tests/gui/m76_write.py
"""
import glob
import os
import subprocess
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
STARTUP_TIMEOUT_S = 60.0
WRITE_ID = "fr.inria.built-in.Write"
CONSTANT_ID = "net.sf.openfx.ConstantPlugin"

try:
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, QRect, Qt, QTimer, Q_ARG
    from PySide6.QtGui import QEnterEvent, QFocusEvent, QGuiApplication, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QDialogButtonBox, QLabel, QLineEdit, QListView, QProgressBar, QProgressDialog, QPushButton, QWidget

    import guitest as gt
    from guitest import check, report, require

    FIXTURE_DIR = os.path.join(gt.OUT, "fixtures", "m76")
    GREEN = (0.1, 0.8, 0.1)
    BLUE = (0.1, 0.1, 0.8)
    RED = (0.8, 0.1, 0.1)
    SEQUENCE = os.path.join(FIXTURE_DIR, "m76-render.###.exr")
    SELECTED = os.path.join(FIXTURE_DIR, "m76-selected.exr")

    def dominant(colour):
        """'red', 'green' or 'blue' when one channel leads the others by a clear margin. The viewer's
        display transform tones values down, so a threshold on the channel itself would not hold."""
        if colour is None:
            return None
        order = sorted(range(3), key=lambda i: -colour[i])
        if colour[order[0]] - colour[order[1]] < 60:
            return None
        return ("red", "green", "blue")[order[0]]

    def viewer_shows(name):
        return dominant(gt.viewer_centre_colour()) == name

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
        from PySide6.QtCore import Q_ARG
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

    def open_node_dialog():
        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        # The Tab shortcut only works while the pointer is over the graph, which the graph learns
        # from its enter event; Xvfb has no pointer to move there.
        centre = QPointF(view.rect().center())
        QApplication.sendEvent(view, QEnterEvent(centre, centre, QPointF(view.mapToGlobal(view.rect().center()))))
        QTest.keyClick(view, Qt.Key_Tab)
        ok = yield ("until", lambda: any(d.isVisible() for d in gt.widgets_of("TabTabTabWidget")), POLL_TIMEOUT_S)
        require(ok, "the Tab menu opens")
        dialog = next(d for d in gt.widgets_of("TabTabTabWidget") if d.isVisible())
        return dialog, dialog.findChildren(QLineEdit)[0], dialog.findChildren(QListView)[0]

    def listed(line_edit, view, text):
        line_edit.setText(text)
        QApplication.processEvents()
        model = view.model()
        return [str(model.index(r, 0).data()) for r in range(model.rowCount())]

    def writers():
        return [n for n in app.getChildren() if n.getPluginID() == WRITE_ID]

    def sequence_files():
        return sorted(glob.glob(os.path.join(FIXTURE_DIR, "m76-render.*.exr")))

    def sizes(paths):
        return [os.path.getsize(p) if os.path.isfile(p) else -1 for p in paths]

    def frame_file(frame):
        return os.path.join(FIXTURE_DIR, "m76-render.%03d.exr" % frame)

    def recolour(path, rgb):
        """Sets every pixel of the RGBA file to rgb with alpha 1."""
        temporary = os.path.join(os.path.dirname(path), "recolour-temporary.exr")
        try:
            subprocess.run(["oiiotool", path, "--mulc", "0,0,0,1", "--addc", "%s,%s,%s,0" % rgb, "-o", temporary],
                           check=True, capture_output=True)
            os.replace(temporary, path)
            return True
        except (OSError, subprocess.CalledProcessError):
            pass
        try:
            import OpenImageIO as oiio
            buf = oiio.ImageBuf(path)
            buf.read(0, 0, True)
            if not oiio.ImageBufAlgo.fill(buf, (rgb[0], rgb[1], rgb[2], 1.0)) or not buf.write(temporary):
                return False
            os.replace(temporary, path)
            return True
        except Exception:
            return False

    def show_node_graph():
        """Brings the node graph's tab back to the front: the Progress panel takes it over when a
        render starts."""
        view = next(iter(gt.widgets_of("NodeGraph")), None)
        require(view is not None, "the node graph exists")
        tabs = view.parentWidget()
        while tabs is not None and gt.class_name(tabs) != "TabWidget":
            tabs = tabs.parentWidget()
        require(tabs is not None, "the node graph sits in a tab widget")
        for index in range(8):
            if view.isVisible():
                break
            QMetaObject.invokeMethod(tabs, "makeCurrentTab", Q_ARG(int, index))
            QApplication.processEvents()
        require(view.isVisible(), "the node graph tab is shown")
        return view

    def panel_button(panel, text):
        found = [b for b in panel.findChildren(QPushButton) if b.isVisible() and b.text() == text]
        return found[0] if len(found) == 1 else None

    progress = {"bars": 0, "peak": 0, "panel_visible": False, "dialogs": 0, "modal": False}

    def sample_progress():
        for panel in gt.widgets_of("ProgressPanel"):
            try:
                bars = panel.findChildren(QProgressBar)
                progress["bars"] = max(progress["bars"], len(bars))
                for bar in bars:
                    progress["peak"] = max(progress["peak"], bar.value())
                progress["panel_visible"] = progress["panel_visible"] or panel.isVisible()
            except RuntimeError:
                continue
        for dialog in QApplication.topLevelWidgets():
            try:
                if isinstance(dialog, QProgressDialog) and dialog.isVisible():
                    progress["dialogs"] += 1
                    progress["modal"] = progress["modal"] or QApplication.activeModalWidget() is dialog
            except RuntimeError:
                continue

    def drive_render_dialog(seen, preset=None):
        """Accepts the RenderDialog the way a user would. The dialog runs its own event loop inside
        whatever opened it, so a repeating timer does the work from within that loop."""
        timer = QTimer()
        timer.setInterval(100)

        def poll():
            dialogs = [d for d in gt.widgets_of("RenderDialog") if d.isVisible()]
            if not dialogs:
                return
            timer.stop()
            dialog = dialogs[0]
            seen["dialog"] = True
            seen["writers"] = [l.text() for l in dialog.findChildren(QLabel) if l.isVisible()]
            combos = [w for w in dialog.findChildren(QWidget) if gt.class_name(w) == "ComboBox"]
            seen["preset"] = str(combos[0].property("natronCurrentText")) if combos else None
            if preset is not None and combos:
                QMetaObject.invokeMethod(combos[0], "setCurrentIndex", Q_ARG(int, preset))
                QApplication.processEvents()
            boxes = [w for w in dialog.findChildren(QDialogButtonBox) if w.isVisible()]
            seen["ok"] = bool(boxes) and boxes[0].button(QDialogButtonBox.Ok) is not None
            if seen["ok"]:
                gt.shot("m76-render-dialog.png")
                boxes[0].button(QDialogButtonBox.Ok).click()
            else:
                dialog.reject()

        timer.timeout.connect(poll)
        timer.start()
        return timer

    sampler = QTimer()
    sampler.setInterval(5)
    sampler.timeout.connect(sample_progress)

    state = {}

    def dialog_section():
        gt.section("create-time save dialog")
        seen = {}

        def inspect():
            dialogs = [d for d in gt.widgets_of("SequenceFileDialog") if d.isVisible()]
            if not dialogs:
                QTimer.singleShot(200, inspect)
                return
            dialog = dialogs[0]
            seen["save_button"] = any(b.text() == "Save" for b in dialog.findChildren(QPushButton))

            def read_menu():
                popup = QApplication.activePopupWidget()
                if popup is None:
                    return
                seen["items"] = [a.text() for a in popup.actions()]
                screen_crop("m76-save-dialog-filter.png",
                            global_rect(dialog).united(global_rect(popup)).adjusted(-10, -10, 10, 10))
                QTest.keyClick(popup, Qt.Key_Escape)

            # The dialog shows its extension list only in the menu behind the filter's drop-down.
            QTimer.singleShot(800, read_menu)
            QMetaObject.invokeMethod(dialog, "showFilterMenu")
            seen["dialog"] = True
            dialog.reject()

        before = len(writers())
        dialog, line_edit, view = yield from open_node_dialog()
        entries = listed(line_edit, view, "Write")
        row = next((r for r, e in enumerate(entries) if e.split("[")[0].strip() == "Write"), -1)
        require(row >= 0, "the Tab menu offers Write (%r)" % entries)
        view.setCurrentIndex(view.model().index(row, 0))
        QTimer.singleShot(500, inspect)
        # Return creates the Write, whose creation opens the modal save dialog; the call returns once
        # inspect() has rejected it.
        QTest.keyClick(line_edit, Qt.Key_Return)
        QApplication.processEvents()
        check(seen.get("dialog", False), "the Write's create-time dialog opened before any node existed")
        check(seen.get("save_button", False), "it is a save dialog (has a Save button)")
        check(len(writers()) == before, "cancelling the dialog creates no Write")
        items = [i.lower() for i in seen.get("items", [])]
        report("NOTE save dialog filter entries: %r" % items)
        check(len(items) > 3, "the dialog's filter menu opened (%d entries)" % len(items))
        check("*.exr" in items, "the dialog's filter lists exr")
        check("*.dpx" in items, "the dialog's filter lists dpx")
        check("*.tif" in items or "*.tiff" in items, "the dialog's filter lists tiff")
        check(not any(i in items for i in ("*.mov", "*.cr2", "*.psd")), "the dialog's filter lists no mov, cr2 or psd")

    def search_section():
        gt.section("node search")
        dialog, line_edit, view = yield from open_node_dialog()
        entries = listed(line_edit, view, "Write")
        report("NOTE search 'Write': %r" % entries)
        check(any(e.split("[")[0].strip() == "Write" for e in entries), "searching Write finds the Write node")
        for name in ("WriteOIIO", "WritePNG"):
            found = listed(line_edit, view, name)
            report("NOTE search %r: %r" % (name, found))
            check(not any(name.lower() in e.lower() for e in found), "searching %s finds no such node" % name)
        QTest.keyClick(line_edit, Qt.Key_Escape)
        QApplication.processEvents()

    def build_graph():
        gt.section("setup")
        os.makedirs(FIXTURE_DIR, exist_ok=True)
        for stale in sequence_files() + [SELECTED]:
            if os.path.isfile(stale):
                os.remove(stale)
        viewer.setPosition(400, 600)
        constant = app.createNode(CONSTANT_ID)
        require(constant is not None, "create a Constant", gt.plugin_problem(CONSTANT_ID))
        constant.setPosition(200, 100)
        constant.getParam("color").set(GREEN[0], GREEN[1], GREEN[2], 1.0)
        write = app.createWriter(SEQUENCE)
        require(write is not None, "create a Write for the sequence")
        require(write.getPluginID() == WRITE_ID, "it is a native Write (%s)" % write.getPluginID())
        write.setPosition(200, 350)
        require(write.connectInput(0, constant), "connect the Constant to the Write")
        write.getParam("frameRange").setValue(2)
        write.getParam("firstFrame").setValue(1)
        write.getParam("lastFrame").setValue(3)
        require(viewer.connectInput(0, write), "connect the Viewer to the Write")
        state.update(viewer=viewer, constant=constant, write=write)
        ok = yield ("until", lambda: viewer_shows("green"), POLL_TIMEOUT_S)
        check(ok, "the viewer shows the Write's input, the Constant (%r)" % (gt.viewer_centre_colour(),))
        yield ("sleep", 1000)
        check(not sequence_files(), "viewing the Write before rendering writes no file (%r)" % sequence_files())

    def panel_section():
        gt.section("Write panel")
        write = state["write"]
        yield from open_panel(write)
        yield ("sleep", 800)
        panel = panel_of(write)
        require(panel is not None, "the Write's panel is open")
        rows = panel_rows(panel)
        report("NOTE panel labels top to bottom: %r" % rows)
        order = ["File", "Overwrite", "Clip To RoD", "Frame Range", "First Frame", "Last Frame",
                 "Frame Increment", "Output Colorspace", "OCIO Context", "Compression", "Write Metadata",
                 "Read back file"]
        ys = [first_y(rows, n) for n in order]
        check(all(y is not None for y in ys) and ys == sorted(ys),
              "the Controls page runs file, overwrite, clip to RoD, range, increment, channels, colourspace, context, options, metadata, read back (%r)" % list(zip(order, ys)))
        sliders = [w for w in panel.findChildren(QWidget) if gt.class_name(w) == "ScaleSliderQt" and w.isVisible()]
        fy, ly = first_y(rows, "First Frame"), first_y(rows, "Last Frame")
        near = [w for w in sliders if fy is not None and ly is not None and fy - 10 <= w.mapTo(panel, w.rect().topLeft()).y() <= ly + 30]
        check(not near, "First Frame and Last Frame have no slider (%d slider(s) in the panel)" % len(sliders))
        render = [b for b in panel.findChildren(QPushButton) if b.isVisible() and b.text() == "Render"]
        check(len(render) == 1, "the panel has one Render button")
        if render:
            rows_y = [y for _, y in rows]
            check(render[0].mapTo(panel, render[0].rect().topLeft()).y() >= max(rows_y), "the Render button is the last row")
        reload = panel_button(panel, "Reload")
        check(reload is not None, "the panel has one Reload button")
        if reload is not None:
            ry = first_y(rows, "Read back file")
            check(ry is not None and abs(reload.mapTo(panel, reload.rect().topLeft()).y() - ry) < 16,
                  "the Reload button sits on the Read back file row")
            check(not reload.isEnabled(), "the Reload button is disabled while read back is off")
        screen_crop("m76-write-panel.png", global_rect(panel))

    def colorspace_section():
        gt.section("output colourspace")
        write = state["write"]
        panel = panel_of(write)
        require(panel is not None, "the Write's panel is open")
        rows = panel_rows(panel)
        combo = knob_combo(panel, first_y(rows, "Output Colorspace"))
        require(combo is not None, "the Output Colorspace menu is on screen")
        items = combo_items(combo)
        default_text = combo_current(combo)
        space = write.getParam("ocioOutputSpace")
        marker = write.getParam("ocioOutputSpaceSet")
        report("NOTE Output Colorspace: %d entries, current %r, first entries %r" % (len(items), default_text, items[:12]))
        check(bool(default_text), "the menu shows a default colourspace for an exr (%r)" % default_text)
        check(not marker.getValue(), "no colourspace is marked as chosen yet")

        menu = {}

        def grab_menu():
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            menu["top"] = popup_actions(popup)
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
            rect = global_rect(combo)
            for w in QApplication.topLevelWidgets():
                try:
                    if w.isVisible() and gt.class_name(w) == "QMenu":
                        rect = rect.united(global_rect(w))
                except RuntimeError:
                    continue
            screen_crop("m76-output-colorspace-menu.png", rect.adjusted(-10, -10, 10, 10))
            while QApplication.activePopupWidget() is not None:
                QApplication.activePopupWidget().close()

        QTimer.singleShot(1200, grab_menu)
        click(combo)
        QApplication.processEvents()
        check(bool(menu.get("top")), "the Output Colorspace menu opened as a popup")
        nested = [n for n, is_menu in menu.get("top", []) if is_menu]
        report("NOTE top-level menu entries: %r" % menu.get("top"))
        check(len(nested) >= 3, "the top level holds nested submenus (%r)" % nested)
        report("NOTE expanded submenu %r: %r (visible=%r)" % (menu.get("sub_name"), menu.get("sub"), menu.get("sub_visible")))
        check(bool(menu.get("sub_visible")), "a submenu expanded")
        check(combo_current(combo) == default_text, "closing the menu without a choice leaves the colourspace alone")

        alt = next((i for i in items if "sRGB" in i and i != default_text), None)
        if alt is None:
            alt = next((i for i in items if i != default_text), None)
        require(alt is not None, "a second colourspace is on the menu")
        require(choose(combo, alt), "pick %s in the menu" % alt)
        yield ("sleep", 500)
        check(combo_current(combo) != default_text, "the menu shows the new choice (%r)" % combo_current(combo))
        check(bool(marker.getValue()), "the choice is marked as made")
        chosen = str(space.getValue())
        check(bool(chosen) and chosen != default_text, "the stored colourspace is the new one (%r)" % chosen)
        screen_crop("m76-colorspace-changed.png", global_rect(panel))

        seen = reset_through_right_click(combo)
        check(seen.get("triggered", False), "the right-click menu offers an enabled Reset to default (%r)" % seen.get("entries"))
        yield ("sleep", 500)
        check(combo_current(combo) == default_text, "Reset to default returns the menu to %r (shows %r)" % (default_text, combo_current(combo)))
        check(not marker.getValue(), "Reset to default clears the chosen mark")
        screen_crop("m76-colorspace-reset.png", global_rect(panel))

        require(choose(combo, alt), "pick %s again" % alt)
        yield ("sleep", 500)
        check(combo_current(combo) == alt or combo_current(combo) != default_text, "reselecting a colourspace after a reset takes it (%r)" % combo_current(combo))
        again = combo_current(combo)
        require(choose(combo, alt), "pick %s a second time in a row" % alt)
        yield ("sleep", 300)
        check(combo_current(combo) == again and bool(marker.getValue()), "picking the same colourspace twice keeps it")

        seen = reset_through_right_click(combo)
        yield ("sleep", 500)
        check(combo_current(combo) == default_text, "a final reset returns to %r" % default_text)

    def render_section():
        gt.section("render from the panel")
        write = state["write"]
        panel = panel_of(write)
        require(panel is not None, "the Write's panel is open")
        buttons = [b for b in panel.findChildren(QPushButton) if b.isVisible() and b.text() == "Render"]
        require(len(buttons) == 1, "the panel has a Render button (%d)" % len(buttons))
        check(not sequence_files(), "no output exists before rendering")
        progress.update(bars=0, peak=0, panel_visible=False, dialogs=0, modal=False)
        sampler.start()
        seen = {}
        driver = drive_render_dialog(seen)
        buttons[0].click()
        driver.stop()
        check(seen.get("dialog", False), "the Render button opens the render dialog")
        check(any(write.getLabel() in text for text in seen.get("writers", [])),
              "the render dialog lists the Write (%r)" % seen.get("writers"))
        check(seen.get("preset") == "Write node range", "the dialog defaults to the Write's own range (%r)" % seen.get("preset"))
        ok = yield ("until", lambda: len(sequence_files()) == 3 and all(s > 0 for s in sizes(sequence_files())), POLL_TIMEOUT_S)
        files = sequence_files()
        check(ok, "the render wrote three non-empty files (%r)" % list(zip([os.path.basename(f) for f in files], sizes(files))))
        yield ("sleep", 1000)
        settled = sizes(sequence_files())
        yield ("sleep", 500)
        check(sizes(sequence_files()) == settled and len(settled) == 3, "the sequence has exactly three files and stopped growing")
        sampler.stop()
        report("NOTE progress seen: %r" % progress)
        check(progress["dialogs"] > 0, "the modal progress dialog appeared while rendering")
        check(progress["modal"], "the progress dialog was the active modal widget")
        ok = yield ("until", lambda: not any(isinstance(d, QProgressDialog) and d.isVisible() for d in QApplication.topLevelWidgets()), POLL_TIMEOUT_S)
        check(ok, "the progress dialog closed once the render finished")
        check(not (write.getPersistentMessage() or ""), "the Write reports no error (%r)" % write.getPersistentMessage())
        gt.shot("m76-render-finished.png")

    def readback_section():
        gt.section("read back")
        write, constant = state["write"], state["constant"]
        gt.close_all_panels()
        written = [(p, os.path.getmtime(p)) for p in sequence_files()]
        constant.getParam("color").set(BLUE[0], BLUE[1], BLUE[2], 1.0)
        ok = yield ("until", lambda: viewer_shows("blue"), POLL_TIMEOUT_S)
        check(ok, "with read back off the viewer shows the input, blue (%r)" % (gt.viewer_centre_colour(),))
        yield ("sleep", 500)
        check([(p, os.path.getmtime(p)) for p in sequence_files()] == written, "viewing the blue input leaves the written files alone")
        write.getParam("readBack").setValue(True)
        yield ("sleep", 1000)
        check(viewer_shows("blue"), "the files were written from the green input, so read back still shows the input, blue (%r)" % (gt.viewer_centre_colour(),))
        check(not write.getParam("startRender").getIsEnabled(), "the Render button is disabled while reading back")
        check(not (write.getPersistentMessage() or ""), "the Write reports no error (%r)" % write.getPersistentMessage())
        constant.getParam("color").set(GREEN[0], GREEN[1], GREEN[2], 1.0)
        ok = yield ("until", lambda: viewer_shows("green"), POLL_TIMEOUT_S)
        check(ok, "setting the input back to green shows the written file again (%r)" % (gt.viewer_centre_colour(),))
        check(not (write.getPersistentMessage() or ""), "the Write reports no error (%r)" % write.getPersistentMessage())
        write.getParam("readBack").setValue(False)
        constant.getParam("color").set(BLUE[0], BLUE[1], BLUE[2], 1.0)
        ok = yield ("until", lambda: viewer_shows("blue"), POLL_TIMEOUT_S)
        check(ok, "turning read back off shows the input again, blue (%r)" % (gt.viewer_centre_colour(),))
        constant.getParam("color").set(GREEN[0], GREEN[1], GREEN[2], 1.0)
        ok = yield ("until", lambda: viewer_shows("green"), POLL_TIMEOUT_S)
        check(ok, "the input is green again (%r)" % (gt.viewer_centre_colour(),))

    def reload_section():
        gt.section("Reload")
        write, constant = state["write"], state["constant"]
        frame = app.timelineGetTime()
        require(1 <= frame <= 3, "the viewer shows a rendered frame (%d)" % frame)
        shown = frame_file(frame)
        check(not write.getParam("reload").getIsEnabled(), "Reload is disabled while read back is off")

        write.getParam("readBack").setValue(True)
        yield ("sleep", 500)
        check(write.getParam("reload").getIsEnabled(), "Reload is enabled while reading back")
        ok = yield ("until", lambda: viewer_shows("green"), POLL_TIMEOUT_S)
        check(ok, "with read back on the viewer shows the written file, green (%r)" % (gt.viewer_centre_colour(),))

        require(recolour(shown, RED), "recolour %s in place" % os.path.basename(shown), "no oiiotool or OpenImageIO")
        yield ("sleep", 1500)
        check(viewer_shows("green"), "without Reload the viewer keeps the image it had (%r)" % (gt.viewer_centre_colour(),))
        show_node_graph()
        yield from open_panel(write)
        yield ("sleep", 500)
        panel = panel_of(write)
        require(panel is not None, "the Write's panel is open")
        reload = panel_button(panel, "Reload")
        require(reload is not None and reload.isEnabled(), "the panel has an enabled Reload button")
        reload.click()
        ok = yield ("until", lambda: viewer_shows("red"), POLL_TIMEOUT_S)
        check(ok, "Reload shows the file changed on disk, red (%r)" % (gt.viewer_centre_colour(),))
        check(not (write.getPersistentMessage() or ""), "the Write reports no error (%r)" % write.getPersistentMessage())
        gt.close_all_panels()

        constant.getParam("color").set(BLUE[0], BLUE[1], BLUE[2], 1.0)
        ok = yield ("until", lambda: viewer_shows("blue"), POLL_TIMEOUT_S)
        check(ok, "an edit upstream stops the file being shown, the input is, blue (%r)" % (gt.viewer_centre_colour(),))
        constant.getParam("color").set(GREEN[0], GREEN[1], GREEN[2], 1.0)
        ok = yield ("until", lambda: viewer_shows("red"), POLL_TIMEOUT_S)
        check(ok, "setting the edit back restores the match and the file shows again, red (%r)" % (gt.viewer_centre_colour(),))

        write.getParam("readBack").setValue(False)
        ok = yield ("until", lambda: viewer_shows("green"), POLL_TIMEOUT_S)
        check(ok, "turning read back off shows the input again, green (%r)" % (gt.viewer_centre_colour(),))
        check(not write.getParam("reload").getIsEnabled(), "Reload is disabled again")

    def render_selected_section():
        gt.section("Render Selected")
        constant = state["constant"]
        guis = gt.widgets_of("Gui")
        require(bool(guis), "the main window exists")
        before = set(n.getScriptName() for n in writers())
        # A single-frame project range: a multi-frame render into a file name without a frame number
        # asks for confirmation first.
        app.getProjectParam("frameRange").set(1, 1)
        app.selectNode(constant, True)
        # Render Selected acts on the graph that last had focus, and there is no window manager to
        # give it any under Xvfb.
        view = show_node_graph()
        QApplication.sendEvent(view, QFocusEvent(QEvent.FocusIn))
        seen = {}

        def fill():
            dialogs = [d for d in gt.widgets_of("SequenceFileDialog") if d.isVisible()]
            if not dialogs:
                QTimer.singleShot(200, fill)
                return
            dialog = dialogs[0]
            seen["dialog"] = True
            edits = dialog.findChildren(QLineEdit)
            edits[0].setText(SELECTED)
            QMetaObject.invokeMethod(dialog, "openSelectedFiles")

        QTimer.singleShot(500, fill)
        render_seen = {}
        driver = drive_render_dialog(render_seen)
        # The Write that Render Selected creates asks for its file first, then for the render dialog;
        # the call returns once fill() and the driver have accepted both.
        QMetaObject.invokeMethod(guis[0], "renderSelectedNode")
        QApplication.processEvents()
        driver.stop()
        check(render_seen.get("dialog", False), "Render Selected opens the render dialog")
        check(seen.get("dialog", False), "Render Selected on a Constant asks where to save")
        created = [n for n in writers() if n.getScriptName() not in before]
        check(len(created) == 1, "Render Selected created one Write (%d)" % len(created))
        require(bool(created), "a new Write exists")
        check(any(created[0].getLabel() in text for text in render_seen.get("writers", [])),
              "the render dialog lists the new Write (%r)" % render_seen.get("writers"))
        check(created[0].getPluginID() == WRITE_ID, "the created node is a native Write (%s)" % created[0].getPluginID())
        check(str(created[0].getParam("filename").getValue()) == SELECTED, "its file is the one chosen (%r)" % created[0].getParam("filename").getValue())
        src = created[0].getInput(0)
        check(src is not None and src.getScriptName() == constant.getScriptName(),
              "the created Write's input is the selected Constant (%r)" % (src.getScriptName() if src else None,))
        ok = yield ("until", lambda: os.path.isfile(SELECTED) and os.path.getsize(SELECTED) > 0, POLL_TIMEOUT_S)
        check(ok, "Render Selected wrote the file")

    # The viewer widget only exists once a Viewer node does, so it is created before waiting for it.
    viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer node exists")

    def steps():
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "viewer widget is up")
        gt.close_all_panels()
        yield from dialog_section()
        yield from search_section()
        yield from build_graph()
        yield from panel_section()
        yield from colorspace_section()
        yield from render_section()
        yield from readback_section()
        yield from reload_section()
        yield from render_selected_section()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
