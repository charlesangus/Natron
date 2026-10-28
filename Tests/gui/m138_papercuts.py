"""GUI checks for the GUI papercuts: the file dialog's history and preview buttons not crashing,
the Preferences window opening centred on the screen's available area, and the colour-picker
defaults (output unless a colour knob's eyedropper is on, with Ctrl+Alt picking the other source).

Sections (all run by default; M138_SECTIONS=file-dialog,placement,picker-defaults,deep-read picks a subset):
  file-dialog      Read's create-time file dialog: into a subfolder, Back, Forward twice (the second
                   lands on a disabled button), the preview toggle with no selection and with a PNG
                   selected, then a scanline, a tiled and a deep tiled EXR with no error, toggled off, dialog
                   closed and reopened, toggled again.
  placement        Preferences is centred within the availableGeometry of the screen it is on.
  picker-defaults  Constant(0.25) -> Grade(x2) in the viewer, then a Read, with the eyedropper of the
                   Grade's offset colour knob off and on.
  deep-read        Importing or dropping a deep EXR creates a DeepRead whose viewer shows the image;
                   a flat EXR still creates a Read.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m138-out -e GUI_TEST_TIMEOUT=300 \
        [-e M138_SECTIONS=file-dialog] -w /home/bosley/git/Natron natron-dev \
        Tests/gui/run-gui-test.sh Tests/gui/m138_papercuts.py build/fast/App/Natron

Screenshots: m138-file-dialog-preview.png, m138-file-dialog-preview-m138-{green-scanline,blue-tiled,deep-red-tiled}.png, m138-picker-01..04 (the info bar in each state).

Like the other scripts, mouse input is sent straight to the widgets as synthetic events; the
file dialog is modal, but the script's timers keep running inside its nested event loop.
"""
import os
import re
import subprocess
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 280
SETTLE_MS = 400
RENDER_MS = 1500
CENTRE_TOL_PX = 3
TOL = 1e-3
ALL_SECTIONS = ("file-dialog", "placement", "picker-defaults", "deep-read")

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QItemSelectionModel, QMimeData, QMetaObject, QPoint, QPointF, Qt, QUrl
    from PySide6.QtGui import QDragEnterEvent, QDropEvent, QEnterEvent, QGuiApplication, QKeyEvent, QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLabel, QLineEdit, QListView, QMainWindow, QTreeView, QWidget

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    FILES_DIR = os.path.abspath(os.path.join(gt.OUT, "m138-files"))
    PNG_NAME = "m138-red.png"
    EXR_FIXTURES = (("m138-green-scanline.exr", None, (0.1, 0.9, 0.1)), ("m138-blue-tiled.exr", (64, 64), (0.1, 0.1, 0.9)),
                    ("m138-deep-red-tiled.exr", (64, 64), (0.9, 0.1, 0.1)))

    wanted = os.environ.get("M138_SECTIONS")
    selected = [s for s in (wanted.split(",") if wanted else ALL_SECTIONS)]

    # ---- helpers --------------------------------------------------------------------------

    def make_deep_exr(path, rgb, width, height, tile):
        """A tiled deep EXR with one opaque sample per pixel. oiiotool only: OIIO's Python deep
        writing needs per-pixel sample setup that is not worth a fallback here."""
        color = ",".join(str(v) for v in rgb) + ",1"
        try:
            subprocess.run(["oiiotool", "--pattern", "constant:color=" + color, "%dx%d" % (width, height), "4",
                            "--chnames", "R,G,B,A", "--ch", "R,G,B,A,Z=1.0", "--deepen",
                            "--tile", str(tile[0]), str(tile[1]), "-o", path],
                           check=True, capture_output=True)
            return True
        except (OSError, subprocess.CalledProcessError):
            return False

    def make_image(path, rgb, width=64, height=48, depth="uint8", tile=None):
        """A solid-colour image; tile=(w, h) writes a tiled file, None a scanline one."""
        os.makedirs(os.path.dirname(path), exist_ok=True)
        color = ",".join(str(v) for v in rgb)
        layout = ["--tile", str(tile[0]), str(tile[1])] if tile else ["--scanline"]
        try:
            subprocess.run(["oiiotool", "--pattern", "constant:color=" + color, "%dx%d" % (width, height), "3",
                            "-d", depth] + layout + ["-o", path],
                           check=True, capture_output=True)
            return True
        except (OSError, subprocess.CalledProcessError):
            pass
        try:
            import OpenImageIO as oiio
            import numpy
            pixels = numpy.empty((height, width, 3), dtype=numpy.float32)
            pixels[:, :] = rgb
            out = oiio.ImageOutput.create(path)
            spec = oiio.ImageSpec(width, height, 3, depth)
            if tile:
                spec.tile_width, spec.tile_height = tile
            ok = out.open(path, spec) and out.write_image(pixels)
            out.close()
            return bool(ok)
        except Exception:
            return False

    def make_fixtures():
        ok = make_image(os.path.join(FILES_DIR, PNG_NAME), (0.9, 0.1, 0.1))
        ok = ok and make_image(os.path.join(FILES_DIR, "sub", "m138-inner.png"), (0.1, 0.1, 0.9))
        # Sizes that are not a multiple of the tile size, so edge tiles are partial.
        for name, tile, rgb in EXR_FIXTURES:
            path = os.path.join(FILES_DIR, name)
            if "-deep-" in name:
                ok = ok and make_deep_exr(path, rgb, 300, 200, tile)
            else:
                ok = ok and make_image(path, rgb, 300, 200, "half", tile)
        return ok

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def visible_of(name):
        return [w for w in gt.widgets_of(name) if w.isVisible()]

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def widget_shot(widget, name):
        if widget is None or not widget.isVisible():
            report("NOTE %s: nothing visible to crop, skipping the shot" % name)
            return
        QApplication.processEvents()
        origin = widget.mapToGlobal(QPoint(0, 0))
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), widget.width(), widget.height())
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def send_mouse(widget, etype, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, modifiers))
        QApplication.processEvents()

    def press(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonPress, pos, button, button, modifiers)

    def release(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseButtonRelease, pos, button, Qt.NoButton, modifiers)

    def move(widget, pos, buttons=Qt.NoButton, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, buttons, modifiers)

    def click(widget, pos, button=Qt.LeftButton, modifiers=Qt.NoModifier):
        press(widget, pos, button, modifiers)
        release(widget, pos, button, modifiers)

    def drag(widget, start, end, button=Qt.LeftButton, modifiers=Qt.NoModifier, steps=4):
        start, end = QPointF(start), QPointF(end)
        press(widget, start, button, modifiers)
        for i in range(1, steps + 1):
            move(widget, start + (end - start) * (i / float(steps)), button, modifiers)
        release(widget, end, button, modifiers)

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    # ---- section: file-dialog -------------------------------------------------------------

    def open_read_dialog():
        """Tab menu -> Read, which opens the create-time file dialog. Returns the dialog."""
        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        # The Tab shortcut only works while the pointer is over the graph, which the graph learns
        # from its enter event; Xvfb has no pointer to move there.
        centre = QPointF(view.rect().center())
        QApplication.sendEvent(view, QEnterEvent(centre, centre, QPointF(view.mapToGlobal(view.rect().center()))))
        QTest.keyClick(view, Qt.Key_Tab)
        ok = yield ("until", lambda: any(d.isVisible() for d in gt.widgets_of("TabTabTabWidget")), POLL_TIMEOUT_S)
        require(ok, "the Tab menu opens")
        menu = next(d for d in gt.widgets_of("TabTabTabWidget") if d.isVisible())
        line_edit, entries = menu.findChildren(QLineEdit)[0], menu.findChildren(QListView)[0]
        line_edit.setText("Read")
        QApplication.processEvents()
        model = entries.model()
        row = next((r for r in range(model.rowCount())
                    if str(model.index(r, 0).data()).split("[")[0].strip() == "Read"), -1)
        require(row >= 0, "the Tab menu offers Read")
        entries.setCurrentIndex(model.index(row, 0))
        QTest.keyClick(line_edit, Qt.Key_Return)
        ok = yield ("until", lambda: bool(visible_of("SequenceFileDialog")), POLL_TIMEOUT_S)
        require(ok, "the create-time file dialog opened")
        yield ("sleep", SETTLE_MS)
        return visible_of("SequenceFileDialog")[0]

    def dialog_button(dialog, name):
        return dialog.findChild(QWidget, name)

    def dialog_line_edit(dialog):
        """The file-name field. FileDialogLineEdit has no Q_OBJECT, so it reports itself as a
        LineEdit, as does the filter field; that one sits beside its drop-down button."""
        found = [w for w in dialog.findChildren(QLineEdit)
                 if gt.class_name(w) == "LineEdit" and w.parentWidget() is not None
                 and not w.parentWidget().findChildren(QAbstractButton)]
        return found[0] if found else None

    def go_to(dialog, path):
        """Types a directory into the dialog's name field and presses Return, as a user does."""
        edit = dialog_line_edit(dialog)
        edit.setText(path.rstrip("/") + "/")
        QApplication.sendEvent(dialog, QKeyEvent(QEvent.KeyPress, Qt.Key_Return, Qt.NoModifier))
        QApplication.processEvents()

    def select_file(dialog, name):
        # SequenceDialogView has no Q_OBJECT, so it reports itself as a QTreeView.
        tree = next((v for v in dialog.findChildren(QTreeView) if v.isVisible()), None)
        if tree is None:
            return False
        model = tree.model()
        for r in range(model.rowCount(tree.rootIndex())):
            index = model.index(r, 0, tree.rootIndex())
            if str(index.data()).startswith(name.rsplit(".", 1)[0]):
                tree.selectionModel().select(index, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
                tree.setCurrentIndex(index)
                QApplication.processEvents()
                return True
        return False

    def preview_viewers(dialog):
        return [t for t in visible_of("ViewerTab") if ancestor_of_class(t, "SequenceFileDialog") is dialog]

    def preview_dominant(dialog, channel):
        """True when the centre of the dialog's preview is clearly the given channel (0 r, 1 g, 2 b)."""
        glv = [g for g in visible_of("ViewerGL") if ancestor_of_class(g, "SequenceFileDialog") is dialog]
        if not glv:
            return False
        centre = glv[0].mapToGlobal(glv[0].rect().center())
        c = QGuiApplication.primaryScreen().grabWindow(0).toImage().pixelColor(centre.x(), centre.y())
        rgb = (c.red(), c.green(), c.blue())
        return all(rgb[channel] > rgb[i] + 30 for i in range(3) if i != channel)

    def preview_is_red(dialog):
        return preview_dominant(dialog, 0)

    def file_dialog_section():
        gt.section("file-dialog")
        require(make_fixtures(), "write the PNG fixtures", "no oiiotool or OpenImageIO")

        dialog = yield from open_read_dialog()
        for name in ("SequenceFileDialogPreviousButton", "SequenceFileDialogNextButton",
                     "SequenceFileDialogTogglePreviewButton"):
            require(dialog_button(dialog, name) is not None, "the dialog has a button named " + name)
        back = dialog_button(dialog, "SequenceFileDialogPreviousButton")
        forward = dialog_button(dialog, "SequenceFileDialogNextButton")
        toggle = dialog_button(dialog, "SequenceFileDialogTogglePreviewButton")

        go_to(dialog, FILES_DIR)
        yield ("sleep", SETTLE_MS)
        go_to(dialog, os.path.join(FILES_DIR, "sub"))
        yield ("sleep", SETTLE_MS)
        check(back.isEnabled(), "Back is enabled after two directory changes")
        back.click()
        yield ("sleep", SETTLE_MS)
        check(forward.isEnabled(), "Forward is enabled after Back")
        forward.click()
        yield ("sleep", SETTLE_MS)
        check(not forward.isEnabled(), "Forward is greyed out at the end of the history")
        forward.click()
        QMetaObject.invokeMethod(dialog, "nextFolder")
        yield ("sleep", SETTLE_MS)
        check(bool(visible_of("SequenceFileDialog")), "the dialog survived Forward twice, the second past the end")

        back.click()
        yield ("sleep", SETTLE_MS)
        check(toggle.isEnabled(), "the preview toggle is enabled")
        toggle.click()
        yield ("sleep", RENDER_MS)
        check(toggle.isChecked(), "the preview toggle is on, with nothing selected")
        check(len(preview_viewers(dialog)) > 0, "the preview viewer is shown inside the dialog")

        check(select_file(dialog, PNG_NAME), "selected " + PNG_NAME + " in the dialog")
        ok = yield ("until", lambda: preview_is_red(dialog), POLL_TIMEOUT_S)
        check(ok, "the preview shows the red PNG")
        widget_shot(dialog, "m138-file-dialog-preview")

        # Each EXR has its own colour, so the previous image still on screen cannot pass the check.
        for name, _, rgb in EXR_FIXTURES:
            check(select_file(dialog, name), "selected " + name + " in the dialog")
            channel = rgb.index(max(rgb))
            ok = yield ("until", lambda: preview_dominant(dialog, channel), POLL_TIMEOUT_S)
            check(ok, "the preview shows " + name)
            yield ("sleep", RENDER_MS)
            errors = [m for m in gt.viewer_messages() if "error" in m.lower()]
            check(not errors, "no error previewing " + name + (": " + "; ".join(errors) if errors else ""))
            widget_shot(dialog, "m138-file-dialog-preview-" + name.rsplit(".", 1)[0])

        toggle.click()
        yield ("sleep", SETTLE_MS)
        check(not toggle.isChecked(), "the preview toggle is off again")
        check(len(preview_viewers(dialog)) == 0, "the preview viewer is hidden")
        dialog.reject()
        yield ("sleep", RENDER_MS)
        check(not visible_of("SequenceFileDialog"), "the dialog closed")

        dialog = yield from open_read_dialog()
        toggle = dialog_button(dialog, "SequenceFileDialogTogglePreviewButton")
        toggle.click()
        yield ("sleep", RENDER_MS)
        check(toggle.isChecked() and len(preview_viewers(dialog)) > 0, "after reopening, the preview toggles on again")
        select_file(dialog, PNG_NAME)
        yield ("sleep", RENDER_MS)
        toggle.click()
        yield ("sleep", SETTLE_MS)
        dialog.reject()
        yield ("sleep", RENDER_MS)
        check(not visible_of("SequenceFileDialog"), "the reopened dialog closed")

    # ---- section: placement ---------------------------------------------------------------

    def placement_section():
        gt.section("placement")
        gui = next(iter(gt.widgets_of("Gui")), None)
        require(gui is not None, "found the Gui widget")
        QMetaObject.invokeMethod(gui, "showSettings")
        ok = yield ("until", lambda: bool(visible_of("PreferencesPanel")), POLL_TIMEOUT_S)
        require(ok, "the Preferences window opened")
        yield ("sleep", SETTLE_MS)
        prefs = visible_of("PreferencesPanel")[0]
        screen = prefs.screen()
        avail = screen.availableGeometry()
        frame = prefs.frameGeometry()
        report("NOTE preferences frame=%r available=%r" % (frame, avail))
        if frame.width() <= avail.width() and frame.height() <= avail.height():
            dx = frame.center().x() - avail.center().x()
            dy = frame.center().y() - avail.center().y()
            check(abs(dx) <= CENTRE_TOL_PX and abs(dy) <= CENTRE_TOL_PX,
                  "Preferences is centred in the screen's available area (offset %d, %d)" % (dx, dy))
            check(avail.contains(frame), "Preferences lies inside the available area")
        else:
            check(frame.topLeft() == avail.topLeft(),
                  "Preferences is larger than the available area, so it sits at its top-left")
        prefs.close()
        yield ("sleep", SETTLE_MS)

    # ---- section: picker-defaults ---------------------------------------------------------

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def set_rgba(param, rgba):
        for d in range(min(4, param.getNumDimensions())):
            param.setValue(rgba[d], d)

    def set_all(param, value):
        for d in range(param.getNumDimensions()):
            param.setValue(value, d)

    def view(viewer, node):
        current = viewer.getInput(0)
        if current is not None and current.getScriptName() == node.getScriptName():
            QApplication.processEvents()
            return
        if current is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "the Viewer now shows " + node.getLabel())
        QApplication.processEvents()

    def viewer_gl():
        found = visible_of("ViewerGL")
        return found[0] if found else None

    def info_bar():
        found = visible_of("InfoViewerWidget")
        return found[0] if found else None

    def strip_tags(text):
        return re.sub(r"<[^>]*>", "", text).replace("&nbsp;", " ").strip()

    def info_labels():
        """(text, visible) of every label in the first visible info bar."""
        bar = info_bar()
        if bar is None:
            return []
        out = []
        for w in QApplication.allWidgets():
            try:
                if gt.class_name(w) == "Label" and ancestor_of_class(w, "InfoViewerWidget") is bar:
                    out.append((w.text(), w.isVisible()))
            except RuntimeError:
                continue
        return out

    def info_rgba():
        for text, visible in info_labels():
            if "d93232" in text and visible:
                nums = re.findall(r"-?\d+\.\d+|-?\d+", strip_tags(text))
                try:
                    return [float(n) for n in nums]
                except ValueError:
                    return None
        return None

    def info_source():
        for text, visible in info_labels():
            t = strip_tags(text)
            if t in ("in", "out"):
                return t if visible else None
        return None

    def rgb_is(values, want):
        return values is not None and len(values) >= 3 and all(abs(values[i] - want[i]) <= TOL for i in range(3))

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

    def eyedropper_of(panel, knob_label):
        """The ColorPickerLabel on the row labelled knob_label in panel."""
        rows = []
        for w in panel.findChildren(QLabel):
            try:
                if w.isVisible() and w.text().strip().rstrip(":") == knob_label:
                    rows.append(w.mapTo(panel, w.rect().center()).y())
            except RuntimeError:
                continue
        if not rows:
            return None
        best = None
        for w in gt.widgets_of("ColorPickerLabel"):
            try:
                if not w.isVisible() or ancestor_of_class(w, "NodeSettingsPanel") is not panel:
                    continue
                dy = abs(w.mapTo(panel, w.rect().center()).y() - rows[0])
                if dy <= 12 and (best is None or dy < best[0]):
                    best = (dy, w)
            except RuntimeError:
                continue
        return best[1] if best else None

    def icon_bytes(label):
        """The label's swatch pixels: the eyedropper icon is drawn over the swatch only while picking is on."""
        img = label.pixmap().toImage()
        return bytes(img.constBits()) if not img.isNull() else b""

    def toggle_eyedropper(label):
        """A press and release on the label, as a click does."""
        centre = QPointF(label.rect().center())
        glob = QPointF(label.mapToGlobal(label.rect().center()))
        for etype, buttons in ((QEvent.MouseButtonPress, Qt.LeftButton), (QEvent.MouseButtonRelease, Qt.NoButton)):
            QApplication.sendEvent(label, QMouseEvent(etype, centre, glob, Qt.LeftButton, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def reading(label):
        rgba, source = info_rgba(), info_source()
        report("NOTE %s: %r %r" % (label, rgba, source))
        return rgba, source

    def expect(label, want_rgb, want_source):
        rgba, source = reading(label)
        check(rgb_is(rgba, want_rgb) and source == want_source,
              "%s reads %.2f, marked %s (got %r %r)" % (label, want_rgb[0], want_source, rgba, source))

    def picker_defaults_section():
        gt.section("picker-defaults")
        require(make_fixtures(), "write the PNG fixtures", "no oiiotool or OpenImageIO")
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        viewer.setPosition(0, 2400)
        gt.close_all_panels()

        c25 = create("Constant", 8000, -300)
        set_rgba(c25.getParam("color"), (0.25, 0.25, 0.25, 1.0))
        gt.close_all_panels()
        g2 = create("Grade", 8000, 0)
        set_all(g2.getParam("multiply"), 2.0)
        require(g2.connectInput(0, c25), "connect the Grade to the Constant")
        view(viewer, g2)
        ok = yield ("until", lambda: panel_of(g2) is not None, POLL_TIMEOUT_S)
        require(ok, "the Grade's panel is open")
        yield ("sleep", RENDER_MS)
        gl = viewer_gl()
        require(gl is not None, "the viewer is visible")
        mid = QPointF(gl.width() / 2.0, gl.height() / 2.0)
        around = (mid - QPointF(20, 20), mid + QPointF(20, 20))
        ctrl, ctrl_alt = Qt.ControlModifier, Qt.ControlModifier | Qt.AltModifier
        rect, rect_alt = Qt.ControlModifier | Qt.ShiftModifier, Qt.ControlModifier | Qt.ShiftModifier | Qt.AltModifier
        offset = g2.getParam("offset")

        # No eyedropper: the output by default, the input with Alt.
        move(gl, mid)
        yield ("sleep", RENDER_MS)
        move(gl, mid + QPointF(1, 0))
        expect("hover without an eyedropper", (0.5, 0.5, 0.5), "out")
        click(gl, mid, Qt.LeftButton, ctrl)
        yield ("sleep", SETTLE_MS)
        expect("Ctrl-click without an eyedropper", (0.5, 0.5, 0.5), "out")
        widget_shot(info_bar(), "m138-picker-01-ctrl-click-out")
        click(gl, mid, Qt.LeftButton, ctrl_alt)
        yield ("sleep", SETTLE_MS)
        expect("Ctrl+Alt-click without an eyedropper", (0.25, 0.25, 0.25), "in")
        widget_shot(info_bar(), "m138-picker-02-ctrl-alt-click-in")
        drag(gl, around[0], around[1], Qt.LeftButton, rect)
        yield ("sleep", SETTLE_MS)
        expect("Ctrl+Shift rectangle without an eyedropper", (0.5, 0.5, 0.5), "out")
        drag(gl, around[0], around[1], Qt.LeftButton, rect_alt)
        yield ("sleep", SETTLE_MS)
        expect("Ctrl+Shift+Alt rectangle without an eyedropper", (0.25, 0.25, 0.25), "in")

        # The Grade's offset eyedropper on: the input by default, the output with Alt.
        # Keep the panel's wrapper alive: PySide invalidates the child wrappers when a parent wrapper is collected.
        grade_panel = panel_of(g2)
        dropper = eyedropper_of(grade_panel, "Offset")
        require(dropper is not None, "found the eyedropper on the Grade's Offset row")
        icon_off = icon_bytes(dropper)
        toggle_eyedropper(dropper)
        check(icon_bytes(dropper) != icon_off, "clicking the label turns its eyedropper on")
        yield ("sleep", SETTLE_MS)
        move(gl, mid)
        move(gl, mid + QPointF(1, 0))
        expect("hover with the eyedropper on", (0.25, 0.25, 0.25), "in")
        click(gl, mid, Qt.LeftButton, ctrl)
        yield ("sleep", SETTLE_MS)
        expect("Ctrl-click with the eyedropper on", (0.25, 0.25, 0.25), "in")
        widget_shot(info_bar(), "m138-picker-03-ctrl-click-in-eyedropper")
        filled = [offset.getValue(d) for d in range(3)]
        check(all(abs(v - 0.25) <= TOL for v in filled), "the pick filled the Offset knob with 0.25 (got %r)" % (filled,))
        set_all(offset, 0.0)
        yield ("sleep", RENDER_MS)
        click(gl, mid, Qt.LeftButton, ctrl_alt)
        yield ("sleep", SETTLE_MS)
        expect("Ctrl+Alt-click with the eyedropper on", (0.5, 0.5, 0.5), "out")
        widget_shot(info_bar(), "m138-picker-04-ctrl-alt-click-out-eyedropper")
        set_all(offset, 0.0)

        # A Read has no input: with the eyedropper on, the pick falls back to the output.
        read = app.createReader(os.path.join(FILES_DIR, PNG_NAME))
        require(read is not None, "create a Read for " + PNG_NAME)
        read.setPosition(8300, -300)
        view(viewer, read)
        yield ("sleep", RENDER_MS)
        check(icon_bytes(dropper) != icon_off, "the eyedropper is still on")
        click(gl, mid, Qt.LeftButton, ctrl)
        yield ("sleep", SETTLE_MS)
        rgba, source = reading("Ctrl-click on a Read with the eyedropper on")
        check(source == "out" and rgba is not None, "a Read with the eyedropper on reads the output, marked out")
        widget_shot(info_bar(), "m138-picker-05-read-eyedropper")
        set_all(offset, 0.0)

        toggle_eyedropper(dropper)
        check(icon_bytes(dropper) == icon_off, "clicking the label again turns the eyedropper off")
        gt.close_all_panels()
        for n in (read, g2, c25):
            n.destroy()
        QApplication.processEvents()

    # ---- section: deep-read ---------------------------------------------------------------

    READ_ID = "Read"
    DEEPREAD_ID = "DeepRead"

    def drop_files(paths):
        gui = next(iter(gt.widgets_of("Gui")), None)
        graph = node_graph_view()
        require(gui is not None and graph is not None, "found the Gui and a node graph to drop on")
        mime = QMimeData()
        mime.setUrls([QUrl.fromLocalFile(p) for p in paths])
        pos = QPointF(graph.mapTo(gui, graph.rect().center()))
        # Qt only delivers a drop to a widget that accepted the preceding drag-enter
        enter = QDragEnterEvent(pos.toPoint(), Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier)
        QApplication.sendEvent(gui, enter)
        event = QDropEvent(pos, Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier)
        QApplication.sendEvent(gui, event)
        QApplication.processEvents()

    def created_since(before):
        return [n for n in app.getChildren() if n.getScriptName() not in before]

    def file_of(node):
        return node.getParam("filename").getValue()

    def deep_read_section():
        gt.section("deep-read")
        require(make_fixtures(), "write the EXR fixtures", "no oiiotool or OpenImageIO")
        deep_path = os.path.join(FILES_DIR, "m138-deep-red-tiled.exr")
        flat_path = os.path.join(FILES_DIR, "m138-green-scanline.exr")
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer exists")

        def shows(node, channel, label):
            view(viewer, node)
            yield ("sleep", RENDER_MS)
            colour = gt.viewer_centre_colour()
            check(colour is not None and all(colour[channel] > colour[i] + 30 for i in range(3) if i != channel),
                  label + " (centre " + repr(colour) + ")")

        def names():
            return set(n.getScriptName() for n in app.getChildren())

        for how in ("import", "drop"):
            before = names()
            if how == "import":
                app.createReader(deep_path)
            else:
                drop_files([deep_path])
            deep = created_since(before)
            check(len(deep) == 1 and deep[0].getPluginID() == DEEPREAD_ID,
                  how + " of a deep EXR creates a DeepRead (got %r)" % [n.getPluginID() for n in deep])
            if deep and deep[0].getPluginID() == DEEPREAD_ID:
                check(file_of(deep[0]) == deep_path, how + ": the DeepRead holds the dropped file")
                yield from shows(deep[0], 0, how + ": the viewer shows the deep image")

            before = names()
            if how == "import":
                app.createReader(flat_path)
            else:
                drop_files([flat_path])
            flat = created_since(before)
            check(len(flat) == 1 and flat[0].getPluginID() == READ_ID,
                  how + " of a flat EXR creates a Read (got %r)" % [n.getPluginID() for n in flat])
            if flat and flat[0].getPluginID() == READ_ID:
                yield from shows(flat[0], 1, how + ": the viewer shows the flat image")
        yield ("sleep", SETTLE_MS)

    # ---- steps ----------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        for name in selected:
            if name == "file-dialog":
                yield from file_dialog_section()
            elif name == "placement":
                yield from placement_section()
            elif name == "picker-defaults":
                yield from picker_defaults_section()
            elif name == "deep-read":
                yield from deep_read_section()
            else:
                check(False, "unknown section " + name)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
