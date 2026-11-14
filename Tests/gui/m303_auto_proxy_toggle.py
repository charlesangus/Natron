"""GUI check of the viewer's Auto Proxy toggle: the toolbar button shows the autoProxyScrubbing
preference, its keyboard shortcut (Ctrl+Shift+P by default) flips the preference and the button
with it, and a click on the button flips both back. A screenshot of the viewer toolbar with the
button checked goes to m303-auto-proxy.png.

Run through Tests/gui/run-gui-test.sh at 1920x1080. Screenshots and results.txt go to
$NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

SETTLE_MS = 400

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QKeyEvent
    from PySide6.QtWidgets import QAbstractButton, QApplication

    import guitest as gt
    from guitest import check, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(200, exit=True, file=_watchdog_file)

    _held = []

    def viewer_tab():
        tabs = [w for w in gt.widgets_of("ViewerTab") if w.isVisible()]
        _held.extend(tabs)
        return tabs[0] if tabs else None

    def auto_proxy_button(tab):
        for b in tab.findChildren(QAbstractButton):
            _held.append(b)
            if b.text() == "Auto Proxy":
                return b
        return None

    def preference():
        return NatronEngine.natron.getSettings().getParam("autoProxyScrubbing").getValue()

    def press_shortcut(tab):
        for etype in (QEvent.KeyPress, QEvent.KeyRelease):
            QApplication.sendEvent(tab, QKeyEvent(etype, Qt.Key_P, Qt.ControlModifier | Qt.ShiftModifier))
        QApplication.processEvents()

    def steps():
        ok = yield ("until", lambda: viewer_tab() is not None, 60.0)
        require(ok, "a viewer is on screen")
        tab = viewer_tab()
        button = auto_proxy_button(tab)
        require(button is not None, "the viewer toolbar has an Auto Proxy button")
        check(button.isCheckable(), "the button is checkable")
        check("Auto Proxy Scrubbing" in (button.toolTip() or ""), "the tooltip names the Auto Proxy Scrubbing setting")
        start = preference()
        check(button.isChecked() == start, "the button shows the preference (%r)" % (start,))

        press_shortcut(tab)
        yield ("sleep", SETTLE_MS)
        check(preference() == (not start), "the shortcut flips the preference")
        check(button.isChecked() == (not start), "the shortcut flips the button")

        button.click()
        yield ("sleep", SETTLE_MS)
        check(preference() == start, "a click flips the preference back")
        check(button.isChecked() == start, "the button follows")

        if not button.isChecked():
            button.click()
            yield ("sleep", SETTLE_MS)
        gt.shot("m303-auto-proxy.png")
        if button.isChecked() != start:
            button.click()
            yield ("sleep", SETTLE_MS)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
