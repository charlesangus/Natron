"""The lens browser a node's lensBrowse button opens: filtered to 50 mm primes it lists only lenses
near 50 mm, a search with no match empties the list and the details, OK writes the chosen lens id into
the node's lens knob as one undoable edit, and the dialog fits a 1920x1080 screen.

The node under test is any plug-in with the shared lens knobs (lensModel, lens, lensBrowse); pass its
id in LENS_BROWSER_PLUGIN.

Usage: LENS_BROWSER_PLUGIN=<plug-in id> Tests/gui/run-gui-test.sh Tests/gui/m463_lens_browser.py <Natron>

Shots: m463_lens_browser.png (the browser filtered to 50 mm primes with a lens selected and its
cross-section) and m463_lens_browser_empty.png (a search with no match).
Results and screenshots go where Tests/gui/run-gui-test.sh puts them; see guitest.py.
"""
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
WATCHDOG_S = 150
SETTLE_MS = 700
SCREEN = (1920, 1080)
FOCAL_MIN = 45.0
FOCAL_MAX = 55.0
NO_MATCH = "no-such-lens-zzqx"
PLUGIN_LENS = os.environ.get("LENS_BROWSER_PLUGIN", "")

try:
    import faulthandler

    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import (QApplication, QDialogButtonBox, QDoubleSpinBox, QLabel, QLineEdit,
                                   QMainWindow, QTableView)

    import guitest as gt
    from guitest import check, require

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def browser():
        shown = [w for w in gt.widgets_of("LensBrowser") if w.isVisible()]
        return shown[0] if shown else None

    def child(dialog, kind, name):
        found = dialog.findChild(kind, name)
        require(found is not None, "the browser has its %s widget" % name)
        return found

    def edit_menu_action(prefix):
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = gt.menu_of(act)
                if menu is not None and act.text().lower().replace("&", "").startswith("edit"):
                    for a in menu.actions():
                        if a.text().lower().replace("&", "").startswith(prefix):
                            return a
        return None

    def listed_ids(table):
        model = table.model()
        return [str(model.index(r, 0).data(Qt.UserRole)) for r in range(model.rowCount())]

    def listed_focals(table):
        model = table.model()
        return [float(model.index(r, 1).data()) for r in range(model.rowCount())]

    def steps():
        require(PLUGIN_LENS != "", "LENS_BROWSER_PLUGIN names a plug-in with the lens knobs")
        node = app.createNode(PLUGIN_LENS, -1, None, {})
        require(node is not None, "create " + PLUGIN_LENS, gt.plugin_problem(PLUGIN_LENS))
        lens = node.getParam("lens")
        model = node.getParam("lensModel")
        browse = node.getParam("lensBrowse")
        require(lens is not None and model is not None and browse is not None,
                "the node has the lensModel, lens and lensBrowse knobs")
        model.set("Database")
        yield ("sleep", SETTLE_MS)
        before = lens.get()

        gt.section("open")
        browse.trigger()
        ok = yield ("until", lambda: browser() is not None, POLL_TIMEOUT_S)
        require(ok, "the lens browser opens from the Browse button")
        dialog = browser()
        check(not dialog.isModal(), "the browser is not modal")
        frame = dialog.frameGeometry()
        check(frame.width() <= SCREEN[0] and frame.height() <= SCREEN[1],
              "the browser fits %dx%d: %dx%d" % (SCREEN[0], SCREEN[1], frame.width(), frame.height()))
        table = child(dialog, QTableView, "lensTable")
        details = child(dialog, QLabel, "lensDetails")
        search = child(dialog, QLineEdit, "lensSearch")
        total = table.model().rowCount()
        check(total >= 995, "the browser lists the whole database: %d lenses" % total)

        gt.section("filter")
        child(dialog, QDoubleSpinBox, "focalMin").setValue(FOCAL_MIN)
        child(dialog, QDoubleSpinBox, "focalMax").setValue(FOCAL_MAX)
        QApplication.processEvents()
        focals = listed_focals(table)
        check(0 < len(focals) < total, "the focal length filter leaves %d of %d lenses" % (len(focals), total))
        check(all(FOCAL_MIN <= f <= FOCAL_MAX for f in focals),
              "every listed lens is between %g and %g mm: %r" % (FOCAL_MIN, FOCAL_MAX, focals[:10]))
        require(len(focals) > 0, "some 50 mm lens is listed")
        table.selectRow(0)
        QApplication.processEvents()
        chosen = listed_ids(table)[0]
        check(chosen in details.text() or chosen.split("/", 1)[-1] in details.text(),
              "the details pane describes the selected lens %s" % chosen)
        yield ("sleep", SETTLE_MS)
        gt.shot("m463_lens_browser.png")

        gt.section("empty-search")
        search.setText(NO_MATCH)
        QApplication.processEvents()
        check(table.model().rowCount() == 0, "a search with no match lists nothing")
        check(details.text() == "", "a search with no match clears the details: %r" % details.text()[:80])
        buttons = dialog.findChild(QDialogButtonBox)
        require(buttons is not None, "the browser has OK and Cancel")
        check(not buttons.button(QDialogButtonBox.Ok).isEnabled(), "OK is disabled with nothing selected")
        gt.shot("m463_lens_browser_empty.png")
        search.setText("")
        QApplication.processEvents()
        ids = listed_ids(table)
        require(chosen in ids, "clearing the search lists the chosen lens again")
        table.selectRow(ids.index(chosen))
        QApplication.processEvents()

        gt.section("confirm")
        buttons.button(QDialogButtonBox.Ok).click()
        ok = yield ("until", lambda: browser() is None, POLL_TIMEOUT_S)
        check(ok, "OK closes the browser")
        check(lens.get() == chosen, "the lens knob holds the chosen id: %r" % lens.get())

        gt.section("undo")
        undo = edit_menu_action("undo")
        if check(undo is not None and undo.isEnabled(), "an enabled Undo is in the Edit menu"):
            undo.trigger()
            QApplication.processEvents()
            check(lens.get() == before, "one undo restores the previous lens %r: %r" % (before, lens.get()))
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
