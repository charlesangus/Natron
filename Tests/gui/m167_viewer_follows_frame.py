"""GUI check that the viewer follows the current frame's metadata (M167.P5.T2).

Graph: Constant "hd" (built-in format HD, 1920x1080, PAR 1) -> RemoveLayers (alpha removed, so RGB)
and Constant "k2" (format m167k2, 2048x1556, PAR 2, RGBA) -> Switch (which keyed 0@1, 1@2) ->
Viewer. So frame 1 is RGB HD at PAR 1 and frame 2 is RGBA 2K at PAR 2.

The timeline goes 1, 2, 1. After each seek, polling with a timeout, the script checks:
  - the info bar's resolution label (ViewerTab::setInfoBarAndViewerResolution): the named
    format and its WxH. A named format carries its PAR in the format, so the name tells PAR 1
    from PAR 2;
  - the viewer's alpha channel menu: no entry with an A channel at frame 1 (RGB), at least one at frame 2;
  - the fps box value (recorded, and equal between the two visits to frame 1: both generators
    take the project frame rate, so it does not differ between frames here).
It then checks the state still holds after a moment, so a late refresh cannot flip it. A cropped
screenshot of the viewer tab is saved at each step.

Not covered: the edge PAR/fps mismatch in NodeGraph15.cpp is a warning dialog shown when a
connection is refused (handleConnectionError), not a tooltip, so there is nothing to read after a
seek; the ViewerGL texture's PAR is not exposed to Python.

Run through Tests/gui/run-gui-test.sh. Screenshots (m167-viewer-<step>.png) and results.txt go to
$NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 280
SETTLE_MS = 400
RENDER_MS = 1500

HD = ("HD", 1920, 1080, 1.0)
K2 = ("m167k2", 2048, 1556, 2.0)

try:
    import NatronEngine
    from PySide6.QtCore import QPoint
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def visible_of(name):
        return [w for w in gt.widgets_of(name) if w.isVisible()]

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if gt.class_name(w) == name:
                return w
            w = w.parentWidget()
        return None

    def viewer_tab():
        tabs = visible_of("ViewerTab")
        return tabs[0] if tabs else None

    def strip_tags(text):
        return re.sub(r"<[^>]*>", "", text).replace("&nbsp;", " ").strip()

    def info_texts():
        """The visible info bar labels' text, tags stripped."""
        tab = viewer_tab()
        if tab is None:
            return []
        out = []
        for w in QApplication.allWidgets():
            try:
                if gt.class_name(w) == "Label" and w.isVisible() and ancestor_of_class(w, "InfoViewerWidget") is not None \
                        and ancestor_of_class(w, "ViewerTab") is tab:
                    out.append(strip_tags(w.text()))
            except RuntimeError:
                continue
        return out

    def format_shown(fmt):
        name, w, h, _par = fmt
        want = "%dx%d" % (w, h)
        return any(name in t and want in t for t in info_texts())

    def tab_combo_items(tip_text):
        """natronItems of the viewer tab's ComboBox whose tooltip has tip_text, or None."""
        tab = viewer_tab()
        if tab is None:
            return None
        for w in QApplication.allWidgets():
            try:
                if gt.class_name(w) in ("ComboBox", "ChannelsComboBox") and tip_text in (w.toolTip() or "") \
                        and ancestor_of_class(w, "ViewerTab") is tab:
                    return [str(i) for i in (w.property("natronItems") or [])]
            except RuntimeError:
                continue
        return None

    def alpha_menu():
        return tab_combo_items("Alpha channel:")

    def has_alpha(items):
        return items is not None and any(i.rsplit(".", 1)[-1] == "A" for i in items)

    def fps_value():
        tab = viewer_tab()
        if tab is None:
            return None
        for w in QApplication.allWidgets():
            try:
                if gt.class_name(w) == "SpinBox" and "Viewer playback framerate" in (w.toolTip() or "") \
                        and ancestor_of_class(w, "ViewerTab") is tab:
                    return w.text()
            except RuntimeError:
                continue
        return None

    def tab_shot(name):
        tab = viewer_tab()
        if tab is None:
            report("NOTE %s: no visible viewer tab to crop" % name)
            return
        QApplication.processEvents()
        origin = tab.mapToGlobal(QPoint(0, 0))
        pix = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), tab.width(), tab.height())
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def create(plugin_id):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        return node

    def make_constant(fmt):
        node = create("Constant")
        extent = node.getParam("extent")
        require(extent is not None, "Constant has an extent parameter")
        extent.setValue(0)
        choice = node.getParam("NatronParamFormatChoice")
        require(choice is not None, "Constant has a format choice")
        options = [str(o) for o in choice.getOptions()]
        index = next((i for i, o in enumerate(options) if o == fmt[0]), -1)
        if index < 0:
            index = next((i for i, o in enumerate(options) if o.split(" ")[0] == fmt[0]), -1)
        require(index >= 0, "the Constant's format menu lists " + fmt[0], repr(options))
        choice.setValue(index)
        return node

    def step_state(frame_label, fmt, want_alpha):
        """Polls until the viewer shows fmt and the alpha menu matches, then checks again after
        a pause so a late refresh from the frame left behind cannot flip it."""
        ok = yield ("until", lambda: format_shown(fmt) and has_alpha(alpha_menu()) == want_alpha, POLL_TIMEOUT_S)
        yield ("sleep", RENDER_MS)
        texts = info_texts()
        items = alpha_menu()
        report("NOTE %s: info labels %r; alpha menu %r; fps %r" % (frame_label, texts, items, fps_value()))
        check(format_shown(fmt),
              "%s: the info bar shows %s %dx%d (PAR %g) (got %r)" % (frame_label, fmt[0], fmt[1], fmt[2], fmt[3], texts))
        check(has_alpha(items) == want_alpha,
              "%s: the alpha channel menu has %s (got %r)" % (frame_label, "an alpha channel" if want_alpha else "no alpha channel", items))
        check(ok, "%s: the viewer settled within %gs" % (frame_label, POLL_TIMEOUT_S))
        other = K2 if fmt is HD else HD
        check(not format_shown(other), "%s: the info bar does not show %s" % (frame_label, other[0]))
        tab_shot("m167-viewer-" + frame_label.replace(" ", "-"))

    def steps():
        ok = yield ("until", lambda: bool(visible_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()

        gt.section("setup")
        app.addFormat("%s %dx%d %g" % (K2[0], K2[1], K2[2], K2[3]))
        hd = make_constant(HD)
        k2 = make_constant(K2)
        remove = create("RemoveLayers")
        switch = create("Switch")
        viewer = app.getNode("Viewer1") or create("Viewer")
        hd.setPosition(0, 0)
        remove.setPosition(0, 120)
        k2.setPosition(200, 0)
        switch.setPosition(100, 240)
        viewer.setPosition(100, 360)

        channels = remove.getParam("channels")
        require(channels is not None, "RemoveLayers has a channels selector")
        channels.setLayer("alpha")
        require(remove.connectInput(0, hd), "connect RemoveLayers to the HD Constant")
        require(switch.connectInput(0, remove), "connect Switch input 0 to RemoveLayers (RGB HD)")
        require(switch.connectInput(1, k2), "connect Switch input 1 to the 2K Constant (RGBA, PAR 2)")
        which = switch.getParam("which")
        require(which is not None, "Switch has a which parameter")
        which.setValueAtTime(0, 1)
        which.setValueAtTime(1, 2)
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, switch), "the Viewer shows the Switch")
        app.clearSelection()
        gt.close_all_panels()
        py_viewer = app.getViewer(viewer.getScriptName())
        require(py_viewer is not None, "the Viewer has a Python wrapper")
        ok = yield ("until", lambda: viewer_tab() is not None and gt.viewer_up(), POLL_TIMEOUT_S)
        require(ok, "the viewer tab is visible")
        yield ("sleep", RENDER_MS)

        gt.section("scrub")
        fps = {}
        for label, frame, fmt, want_alpha in (("frame 1", 1, HD, False),
                                              ("frame 2", 2, K2, True),
                                              ("back at frame 1", 1, HD, False)):
            py_viewer.seek(frame)
            yield from step_state(label, fmt, want_alpha)
            fps[label] = fps_value()
        check(fps["frame 1"] == fps["back at frame 1"] and fps["frame 1"] is not None,
              "the fps box is the same on both visits to frame 1 (%r vs %r; frame 2 %r)"
              % (fps["frame 1"], fps["back at frame 1"], fps["frame 2"]))
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
