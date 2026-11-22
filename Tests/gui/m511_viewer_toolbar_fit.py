"""The viewer's first toolbar row fits the default viewer pane at 1920x1080, scale 1.

With the default layout in a 1920x1080 window the script finds the first row of the visible
ViewerTab (the row that holds the A: label), prints each control's geometry, sizeHint and
minimumSizeHint plus the row and pane widths, and asserts:
  - the row is no wider than its ViewerTab, and its layout needs no more width than it has;
  - every control lies inside the row;
  - no control overlaps its neighbour;
  - the B combo, the last control, is fully visible.
It shoots the viewer toolbar once. Before the fix the assertions are expected to fail.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 60.0
SETTLE_MS = 1500

try:
    from PySide6.QtCore import QPoint
    from PySide6.QtWidgets import QApplication, QWidget

    from guitest import check, class_name, finish, report, require, run, section, shot, viewer_up, widgets_of

    viewer = app.getNode("Viewer1") or app.createNode("Viewer")
    require(viewer is not None, "a Viewer node exists")

    _held = []

    def hold(w):
        if w is not None:
            _held.append(w)
        return w

    def main_gui():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        return hold(guis[0]) if guis else None

    def first_row():
        """The visible ViewerTab and its first settings row, the parent of the A: label."""
        for tab in widgets_of("ViewerTab"):
            if not tab.isVisible():
                continue
            hold(tab)
            for label in tab.findChildren(QWidget):
                try:
                    if class_name(label) == "Label" and label.isVisible() and label.text() == "A:":
                        return tab, hold(label.parentWidget())
                except RuntimeError:
                    continue
        return None, None

    def describe(w):
        tip = re.sub(r"<[^>]+>", " ", w.toolTip() or "")
        tip = " ".join(tip.split())[:28]
        text = ""
        if hasattr(w, "text"):
            try:
                text = w.text()
            except Exception:
                text = ""
        return "%s %r %r" % (class_name(w), text, tip)

    def steps():
        gui = main_gui()
        if gui is not None:
            gui.resize(1920, 1080)
        ok = yield ("until", viewer_up, POLL_TIMEOUT_S)
        require(ok, "a viewer is up")
        yield ("sleep", SETTLE_MS)

        tab, row = first_row()
        require(row is not None, "found the first viewer row (the parent of the A: label)")

        section("first row")
        row_w = row.width()
        tab_w = tab.width()
        hint = row.sizeHint().width()
        min_hint = row.minimumSizeHint().width()
        report("NOTE ViewerTab width %d, row width %d, row sizeHint %d, row minimumSizeHint %d"
               % (tab_w, row_w, hint, min_hint))

        parts = []
        for w in row.children():
            try:
                if not isinstance(w, QWidget) or not w.isVisible():
                    continue
            except RuntimeError:
                continue
            parts.append(hold(w))
        parts.sort(key=lambda w: w.x())
        require(len(parts) > 0, "the first row has visible controls")
        for w in parts:
            report("NOTE x=%4d w=%4d right=%4d sizeHint=%4d minHint=%4d  %s"
                   % (w.x(), w.width(), w.x() + w.width(), w.sizeHint().width(), w.minimumSizeHint().width(),
                      describe(w)))
        needed = max((w.x() + w.width() for w in parts), default=0)
        report("NOTE controls need %d px; row is %d px; overflow %d px" % (needed, row_w, max(0, needed - row_w)))

        check(row_w <= tab_w, "the row (%d) is no wider than its ViewerTab (%d)" % (row_w, tab_w))
        check(hint <= row_w, "the row's layout sizeHint (%d) fits the row (%d)" % (hint, row_w))
        for w in parts:
            right = w.x() + w.width()
            check(w.x() >= 0 and right <= row_w, "inside the row: %s (x %d..%d of %d)" % (describe(w), w.x(), right, row_w))
        for left, right in zip(parts, parts[1:]):
            check(left.x() + left.width() <= right.x(),
                  "no overlap: %s (right %d) and %s (left %d)" % (describe(left), left.x() + left.width(),
                                                                   describe(right), right.x()))

        b_combo = next((w for w in reversed(parts) if class_name(w) == "ComboBox"), None)
        check(b_combo is not None, "the B combo is the row's last combo box")
        if b_combo is not None:
            top_left = b_combo.mapTo(tab, QPoint(0, 0))
            check(top_left.x() + b_combo.width() <= tab_w,
                  "the B combo's right edge (%d) is inside the ViewerTab (%d)" % (top_left.x() + b_combo.width(), tab_w))
            check(b_combo.width() >= b_combo.minimumSizeHint().width(),
                  "the B combo (%d) is at least its minimumSizeHint (%d)" % (b_combo.width(), b_combo.minimumSizeHint().width()))

        QApplication.processEvents()
        shot("m511-viewer-toolbar-default.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
