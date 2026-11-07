"""The Read and Write panels after the knob cleanup: the Read shows one reload, its Reload button,
which picks up a file rewritten on disk, and no reload icon beside File or Proxy File; changing the
project's default for 8-bit files re-renders a Read on Project default in the viewer; the Write has
no Format Type or format widgets, since it always writes its input's format, and no rewrite icon
beside its File; the OCIOFileTransform and OCIOCDLTransform panels have their Reload button on the
same row as the File field, with no reload icon of the file knob.

A TGA is written at run time with oiiotool (or OpenImageIO's Python module if that is missing)
under $NATRON_GUI_TEST_OUT/fixtures/m223.

Screenshots (cropped): m223-read-panel.png, m223-write-panel.png and m223-<node>-panel.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m223_read_write_knobs.py
"""
import os
import subprocess
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
READ_ID = "Read"
WRITE_ID = "Write"
CONSTANT_ID = "Constant"
GREEN = (0.1, 0.8, 0.1)
BLUE = (0.1, 0.1, 0.8)
FILE_RELOAD_TOOLTIP = "Reload the file"
FILE_REWRITE_TOOLTIP = "Rewrite the file"
OCIO_NODES = ("OCIOFileTransform", "OCIOCDLTransform")

try:
    from PySide6.QtCore import QMetaObject, QRect
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QMainWindow, QPushButton

    import guitest as gt
    from guitest import check, report, require

    FIXTURE_DIR = os.path.join(gt.OUT, "fixtures", "m223")
    READ_PATH = os.path.join(FIXTURE_DIR, "m223-reload.tga")
    WRITE_PATH = os.path.join(FIXTURE_DIR, "m223-write.exr")

    # PySide wrappers of the panels and nodes stay referenced here while the script inspects them.
    state = {}

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
        """'red', 'green' or 'blue' when one channel leads the others by a clear margin."""
        if colour is None:
            return None
        order = sorted(range(3), key=lambda i: -colour[i])
        if colour[order[0]] - colour[order[1]] < 60:
            return None
        return ("red", "green", "blue")[order[0]]

    def viewer_shows(name):
        return dominant(gt.viewer_centre_colour()) == name

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

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

    def panel_rows(panel):
        """(label text, y in the panel) for every visible label, top to bottom."""
        rows = []
        for w in panel.findChildren(QLabel):
            try:
                if w.isVisible() and w.text().strip():
                    rows.append((w.text().strip().rstrip(":"), w.mapTo(panel, w.rect().topLeft()).y()))
            except RuntimeError:
                continue
        return sorted(rows, key=lambda r: r[1])

    def visible_buttons(panel):
        found = []
        for b in panel.findChildren(QPushButton):
            try:
                if b.isVisible():
                    found.append(b)
            except RuntimeError:
                continue
        return found

    def screen_crop(name, widget):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(widget.mapToGlobal(widget.rect().topLeft()), widget.size())
        rect = rect.intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(gt.OUT, name)
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    viewer = app.getNode("Viewer1") or app.createNode("Viewer")
    require(viewer is not None, "a Viewer node exists")
    viewer.setPosition(400, 600)
    state["viewer"] = viewer

    def read_section():
        gt.section("Read panel")
        require(make_solid(READ_PATH, GREEN), "write a run-time TGA", "no oiiotool or OpenImageIO")
        read = app.createReader(READ_PATH)
        require(read is not None, "create a Read")
        require(read.getPluginID() == READ_ID, "it is a native Read (%s)" % read.getPluginID())
        read.setPosition(0, 100)
        state["read"] = read
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, read), "connect the Viewer to the Read")
        ok = yield ("until", lambda: viewer_shows("green"), POLL_TIMEOUT_S)
        check(ok, "the Read displays its file, green (%r)" % (gt.viewer_centre_colour(),))

        require(make_solid(READ_PATH, BLUE), "rewrite the TGA in blue", "no oiiotool or OpenImageIO")
        yield ("sleep", 1500)
        check(viewer_shows("green"), "without Reload the viewer keeps the image it had (%r)" % (gt.viewer_centre_colour(),))

        yield from open_panel(read)
        yield ("sleep", 800)
        panel = panel_of(read)
        require(panel is not None, "the Read's panel is open")
        state["read_panel"] = panel
        buttons = visible_buttons(panel)
        state["read_buttons"] = buttons
        reloads = [b for b in buttons if b.text() == "Reload"]
        icons = [b for b in buttons if FILE_RELOAD_TOOLTIP in (b.toolTip() or "")]
        report("NOTE panel labels top to bottom: %r" % panel_rows(panel))
        check(len(reloads) == 1, "the Read panel has one Reload button (%d)" % len(reloads))
        check(not icons, "no file knob shows its own reload icon (%d)" % len(icons))
        screen_crop("m223-read-panel.png", panel)
        require(len(reloads) == 1, "a single Reload button to click")
        reloads[0].click()
        ok = yield ("until", lambda: viewer_shows("blue"), POLL_TIMEOUT_S)
        check(ok, "Reload displays the rewritten file, blue (%r)" % (gt.viewer_centre_colour(),))
        check(not (read.getPersistentMessage() or ""), "the Read reports no error (%r)" % read.getPersistentMessage())
        gt.close_all_panels()

        file_default = app.getProjectParam("colorSpace8Bit")
        require(file_default is not None, "the project has an 8-bit file default")
        original = file_default.get()
        options = [str(file_default.getOption(i)) for i in range(file_default.getNumOptions())]
        linear = [i for i, name in enumerate(options) if name.startswith("Linear Rec.709")]
        require(bool(linear) and linear[0] != original, "the 8-bit file default can move to a linear space (%r)" % (options,))
        shown = gt.viewer_centre_colour()
        file_default.set(linear[0])
        ok = yield ("until", lambda: gt.viewer_centre_colour() != shown, POLL_TIMEOUT_S)
        check(ok, "a new 8-bit file default re-renders the Read (%r, was %r)" % (gt.viewer_centre_colour(), shown))
        file_default.set(original)
        ok = yield ("until", lambda: gt.viewer_centre_colour() == shown, POLL_TIMEOUT_S)
        check(ok, "restoring the 8-bit file default restores the image (%r, was %r)" % (gt.viewer_centre_colour(), shown))

    def write_section():
        gt.section("Write panel")
        constant = app.createNode(CONSTANT_ID)
        require(constant is not None, "create a Constant", gt.plugin_problem(CONSTANT_ID))
        constant.setPosition(200, 100)
        state["constant"] = constant
        write = app.createWriter(WRITE_PATH)
        require(write is not None, "create a Write")
        require(write.getPluginID() == WRITE_ID, "it is a native Write (%s)" % write.getPluginID())
        write.setPosition(200, 350)
        state["write"] = write
        require(write.connectInput(0, constant), "connect the Constant to the Write")
        check(write.getParam("formatType") is None, "the Write has no formatType parameter")
        check(write.getParam("clipToRoD") is not None, "the Write keeps clipToRoD")

        yield from open_panel(write)
        yield ("sleep", 800)
        panel = panel_of(write)
        require(panel is not None, "the Write's panel is open")
        state["write_panel"] = panel
        labels = [t for t, _ in panel_rows(panel)]
        report("NOTE panel labels top to bottom: %r" % labels)
        check("Clip To RoD" in labels, "the panel shows Clip To RoD")
        gone = [t for t in labels if t in ("Format Type", "Format", "Size", "Pixel Aspect Ratio")]
        check(not gone, "the panel has no format widgets (%r)" % gone)
        rewrite = [b for b in visible_buttons(panel) if FILE_REWRITE_TOOLTIP in (b.toolTip() or "")]
        reload_icons = [b for b in visible_buttons(panel) if FILE_RELOAD_TOOLTIP in (b.toolTip() or "")]
        check(not rewrite, "the Write's File shows no rewrite icon (%d)" % len(rewrite))
        check(not reload_icons, "the Write's File shows no reload icon (%d)" % len(reload_icons))
        screen_crop("m223-write-panel.png", panel)
        gt.close_all_panels()

    def ocio_section():
        gt.section("OCIO file nodes")
        for i, plugin in enumerate(OCIO_NODES):
            node = app.createNode(plugin)
            require(node is not None, "create %s" % plugin, gt.plugin_problem(plugin))
            node.setPosition(600 + 200 * i, 100)
            state["ocio" + plugin] = node
            yield from open_panel(node)
            yield ("sleep", 800)
            panel = panel_of(node)
            require(panel is not None, "%s's panel is open" % plugin)
            state["panel" + plugin] = panel
            reloads = [b for b in visible_buttons(panel) if b.text() == "Reload"]
            icons = [b for b in visible_buttons(panel) if FILE_RELOAD_TOOLTIP in (b.toolTip() or "")]
            fields = [e for e in panel.findChildren(QLineEdit) if e.isVisible() and e.placeholderText() == "File path..."]
            report("NOTE %s panel labels top to bottom: %r" % (plugin, panel_rows(panel)))
            check(len(reloads) == 1, "%s has one Reload button (%d)" % (plugin, len(reloads)))
            check(not icons, "%s's File shows no reload icon (%d)" % (plugin, len(icons)))
            check(len(fields) >= 1, "%s has a File field (%d path fields)" % (plugin, len(fields)))
            if len(reloads) == 1 and fields:
                reload_y = reloads[0].mapTo(panel, reloads[0].rect().center()).y()
                same_row = [f for f in fields if abs(f.mapTo(panel, f.rect().center()).y() - reload_y) <= 6]
                check(len(same_row) == 1, "%s's Reload is on the File row (%d path fields share its row)" % (plugin, len(same_row)))
            screen_crop("m223-%s-panel.png" % plugin.lower(), panel)
            gt.close_all_panels()

    def steps():
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "viewer widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()
        yield from read_section()
        yield from write_section()
        yield from ocio_section()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
