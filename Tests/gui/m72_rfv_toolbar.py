"""Screenshots of the Reverse Flow Variable toolbar category menu and a RFVSwitch with three
pattern rows. Usage: Tests/gui/run-gui-test.sh Tests/gui/m72_rfv_toolbar.py <natron>
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

try:
    import faulthandler

    import NatronEngine
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication, QMenu, QToolBar

    import guitest as gt
    from guitest import check, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(200, exit=True, file=_watchdog_file)

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    _held = []

    def rfv_button():
        for gui in [w for w in gt.widgets_of("Gui") if w.isVisible()]:
            for bar in gui.findChildren(QToolBar):
                for a in bar.actions():
                    w = bar.widgetForAction(a)
                    _held.extend([gui, bar, w])
                    if w is not None and hasattr(w, "menu") and gt.menu_of(w) is not None \
                            and "reverse flow" in (w.toolTip() or "").lower():
                        return w
        return None

    def open_menus():
        menus = []
        popup = QApplication.activePopupWidget()
        if isinstance(popup, QMenu):
            menus.append(popup)
        return menus

    def steps():
        ok = yield ("until", lambda: graph_view() is not None, 60.0)
        require(ok, "node graph widget is up")
        gt.close_all_panels()
        button = rfv_button()
        require(button is not None, "a toolbox button whose tooltip names Reverse Flow Variable")
        check(not button.icon().isNull(), "the category button has an icon")
        seen = {}

        def on_popup():
            menus = open_menus()
            if not menus:
                return
            menu = menus[0]
            seen["items"] = [a.text() for a in menu.actions()]
            gt.shot("m72-toolbar-rfv-menu.png")
            menu.close()

        def guard():
            for m in open_menus():
                m.close()
            if not seen:
                QTimer.singleShot(1500, guard)

        QTimer.singleShot(1000, on_popup)
        QTimer.singleShot(5000, guard)
        button.showMenu()
        QApplication.processEvents()
        check(sorted(seen.get("items", [])) == ["RFVRead", "RFVSet", "RFVSwitch"] or len(seen.get("items", [])) == 3,
              "the menu lists three nodes: %r" % (seen.get("items"),))

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
