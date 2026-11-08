"""Checks app.newUndoStep() and app.noUndoStep() from a Python panel's PySide button handlers: a
handler that wraps knob edits in app.newUndoStep("Panel Edit") leaves one entry, which the Edit
menu offers as "Undo Panel Edit" and which undoes and redoes all its edits in one step; a handler
that raises inside its step still leaves one entry holding the edits made before the exception;
a handler's app.noUndoStep() block leaves no entry.

Run with NatronRenderer instead, the same script makes the same edits with no undo history and
must complete without error.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    NATRON_GUI_TEST_OUT=build/m197-gui/out Tests/gui/run-gui-test.sh Tests/gui/m197_python_undo_step.py build/release/App/Natron
    NATRON_GUI_TEST_OUT=build/m197-gui/bg build/release/App/NatronRenderer Tests/gui/m197_python_undo_step.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 200
SETTLE_MS = 300
BLUR_ID = "Blur"

try:
    import faulthandler

    import NatronEngine

    import guitest as gt
    from guitest import check

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def knob_state(blur):
        size = blur.getParam("size")
        return (size.getValue(0), size.getValue(1), blur.getParam("cropToFormat").getValue())

    def edit_in_step(blur, label, raise_after=False):
        with app.newUndoStep(label):
            blur.getParam("size").setValue(4.0, 0)
            blur.getParam("size").setValue(5.0, 1)
            blur.getParam("cropToFormat").setValue(not blur.getParam("cropToFormat").getValue())
            if raise_after:
                raise RuntimeError("m197 handler failure")

    def edit_unrecorded(blur):
        with app.noUndoStep():
            blur.getParam("size").setValue(7.0, 0)

    def make_blur():
        blur = app.createNode(BLUR_ID)
        blur.getParam("size").setValue(1.0, 0)
        blur.getParam("size").setValue(1.0, 1)
        blur.getParam("cropToFormat").setValue(True)
        return blur

    def run_background():
        blur = make_blur()
        before = knob_state(blur)
        edit_in_step(blur, "Background Edit")
        check(knob_state(blur) == (4.0, 5.0, False),
              "background: the step's edits are applied (got %r, was %r)" % (knob_state(blur), before))
        try:
            edit_in_step(blur, "Background Fail", raise_after=True)
            check(False, "background: the exception propagates out of the step")
        except RuntimeError as e:
            check("m197 handler failure" in str(e), "background: the exception propagates out of the step")
        edit_unrecorded(blur)
        check(blur.getParam("size").getValue(0) == 7.0, "background: the noUndoStep edit is applied")
        gt.finish()

    def run_gui():
        from PySide6.QtWidgets import QApplication, QMainWindow, QPushButton
        import NatronGui

        def edit_menu_actions():
            for win in QApplication.topLevelWidgets():
                if not isinstance(win, QMainWindow) or win.menuBar() is None:
                    continue
                for act in win.menuBar().actions():
                    menu = gt.menu_of(act)
                    if menu is not None and act.text().replace("&", "").lower().startswith("edit"):
                        return menu.actions()
            return []

        def history_action(prefix):
            for a in edit_menu_actions():
                if a.text().replace("&", "").startswith(prefix):
                    return a
            return None

        def undo_text():
            a = history_action("Undo")
            return a.text().replace("&", "") if a is not None and a.isEnabled() else ""

        def trigger(prefix):
            a = history_action(prefix)
            if not check(a is not None and a.isEnabled(), "an enabled %s action is in the Edit menu" % prefix):
                return
            a.trigger()
            QApplication.processEvents()

        # The handlers, the panel and its buttons stay referenced for the whole run: PySide drops a
        # child widget's wrapper with its parent's, which would disconnect the handlers.
        keep = {}

        def steps():
            yield ("sleep", SETTLE_MS)
            gt.close_all_panels()
            blur = make_blur()
            yield ("sleep", SETTLE_MS)
            baseline = undo_text()
            before = knob_state(blur)

            panel = NatronGui.PyPanel("m197UndoStepPanel", "Undo Step Panel", False, app)
            edit_button = QPushButton("Edit")
            fail_button = QPushButton("Fail")
            quiet_button = QPushButton("Quiet")
            panel.addWidget(edit_button)
            panel.addWidget(fail_button)
            panel.addWidget(quiet_button)
            failures = []

            def on_edit():
                edit_in_step(blur, "Panel Edit")

            def on_fail():
                try:
                    edit_in_step(blur, "Panel Fail", raise_after=True)
                except RuntimeError as e:
                    failures.append(str(e))

            def on_quiet():
                edit_unrecorded(blur)

            edit_button.clicked.connect(on_edit)
            fail_button.clicked.connect(on_fail)
            quiet_button.clicked.connect(on_quiet)
            keep.update(panel=panel, buttons=(edit_button, fail_button, quiet_button),
                        handlers=(on_edit, on_fail, on_quiet))
            panel.show()
            yield ("sleep", SETTLE_MS)

            gt.section("handler step")
            edit_button.click()
            QApplication.processEvents()
            check(knob_state(blur) == (4.0, 5.0, False), "the handler's edits are applied (got %r)" % (knob_state(blur),))
            check(undo_text() == "Undo Panel Edit",
                  "the Edit menu offers \"Undo Panel Edit\" (got %r, was %r)" % (undo_text(), baseline))
            trigger("Undo")
            check(knob_state(blur) == before, "one undo reverts all the handler's edits (got %r, want %r)" % (knob_state(blur), before))
            check(undo_text() == baseline, "after the undo the Edit menu offers what it did before (got %r, want %r)" % (undo_text(), baseline))
            trigger("Redo")
            check(knob_state(blur) == (4.0, 5.0, False), "one redo reapplies all the handler's edits (got %r)" % (knob_state(blur),))
            check(undo_text() == "Undo Panel Edit", "after the redo the Edit menu offers \"Undo Panel Edit\" again (got %r)" % (undo_text(),))
            trigger("Undo")

            gt.section("handler exception")
            fail_button.click()
            QApplication.processEvents()
            check(failures == ["m197 handler failure"], "the exception propagates out of the step (got %r)" % (failures,))
            check(knob_state(blur) == (4.0, 5.0, False), "the edits made before the exception stay (got %r)" % (knob_state(blur),))
            check(undo_text() == "Undo Panel Fail", "the failed handler leaves one \"Undo Panel Fail\" entry (got %r)" % (undo_text(),))
            trigger("Undo")
            check(knob_state(blur) == before, "one undo reverts the failed handler's edits (got %r)" % (knob_state(blur),))

            gt.section("handler noUndoStep")
            quiet_button.click()
            QApplication.processEvents()
            check(blur.getParam("size").getValue(0) == 7.0, "the noUndoStep edit is applied")
            check(undo_text() == baseline, "the noUndoStep handler leaves no entry (got %r, want %r)" % (undo_text(), baseline))

            panel.close()
            faulthandler.cancel_dump_traceback_later()

        gt.run(steps())

    if NatronEngine.natron.isBackground():
        run_background()
    else:
        run_gui()
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
