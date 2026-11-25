"""Checks that the Raw and Loop node-toolbar category buttons carry their own icons rather than the
default puzzle icon, and saves a screenshot of the toolbar. Usage:
Tests/gui/run-gui-test.sh Tests/gui/toolbar_group_icons.py <natron>
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QSize
    from PySide6.QtGui import QIcon, QImage
    from PySide6.QtWidgets import QToolBar

    import guitest as gt
    from guitest import check, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(200, exit=True, file=_watchdog_file)

    ICON_SIZE = QSize(24, 24)
    _held = []

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def toolbox_buttons():
        buttons = []
        for gui in [w for w in gt.widgets_of("Gui") if w.isVisible()]:
            for bar in gui.findChildren(QToolBar):
                for a in bar.actions():
                    w = bar.widgetForAction(a)
                    _held.extend([gui, bar, w])
                    if w is not None and hasattr(w, "menu") and gt.menu_of(w) is not None:
                        buttons.append(w)
        return buttons

    def find_button(buttons, label):
        for b in buttons:
            if (b.toolTip() or "").strip().lower() == label.lower():
                return b
        for b in buttons:
            if label.lower() in (b.toolTip() or "").lower():
                return b
        return None

    def alpha_mask(icon):
        return icon.pixmap(ICON_SIZE).toImage().convertToFormat(QImage.Format_Alpha8)

    def mask_bytes(icon):
        return bytes(alpha_mask(icon).constBits())

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, 60.0)
        require(ok, "node graph widget is up")
        gt.close_all_panels()
        buttons = toolbox_buttons()
        require(len(buttons) > 0, "the node toolbox has category buttons")
        raw = find_button(buttons, "Raw")
        loop = find_button(buttons, "Loop")
        require(raw is not None, "a toolbox button for the Raw category")
        require(loop is not None, "a toolbox button for the Loop category")
        puzzle = QIcon(":/Resources/Icons/lucide/puzzle.svg")
        puzzle_mask = mask_bytes(puzzle)
        raw_mask = mask_bytes(raw.icon())
        loop_mask = mask_bytes(loop.icon())
        check(not raw.icon().isNull(), "the Raw button has an icon")
        check(not loop.icon().isNull(), "the Loop button has an icon")
        check(raw_mask != puzzle_mask, "the Raw icon shape differs from the puzzle shape")
        check(loop_mask != puzzle_mask, "the Loop icon shape differs from the puzzle shape")
        check(raw_mask != loop_mask, "the Raw and Loop icons differ from each other")
        names = [(b.toolTip() or "") for b in buttons]
        raw_pos = buttons.index(raw)
        image_pos = [i for i, n in enumerate(names) if n.strip().lower() == "image"]
        if image_pos:
            check(raw_pos == image_pos[0] + 1, "Raw sits right after Image in the toolbox")
        gt.shot("toolbar-group-icons.png")
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
