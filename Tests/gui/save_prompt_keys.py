"""Question dialogs answer to one plain letter per button, underlined in the button text.

The checks, in order:
  labels         the save prompt's buttons read &Save, &Discard and &Cancel
  cancel         with the project edited after a save, closing it and pressing c leaves it open, its
                 nodes in place and its file untouched; Escape does the same
  discard        pressing d closes the project without writing its file
  save           pressing s writes the file and closes the project
  yes/no         a Yes/No question returns Yes for y and No for n
  message box    Natron's own message box (a long information dialog) shows &OK and closes on o

The project counts as unsaved only once an auto-save has followed an edit, so each edit waits for
the auto-save before the prompt is raised. Every modal dialog is raised from a single-shot timer,
so this generator keeps running inside the dialog's event loop (see run() in guitest.py).

Usage (natron-dev container):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        Tests/gui/run-gui-test.sh Tests/gui/save_prompt_keys.py <Natron.AppImage>'
"""
import os
import shutil
import sys
import time
import traceback
import zipfile

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 240
DIALOG_TIMEOUT_S = 20.0
SETTLE_MS = 700
AUTOSAVE_DELAY_S = 1
AUTOSAVE_WAIT_MS = 4000
CONSTANT = "net.sf.openfx.ConstantPlugin"

try:
    import NatronEngine
    import NatronGui
    from PySide6.QtCore import QEvent, Qt, QTimer
    from PySide6.QtGui import QKeyEvent
    from PySide6.QtWidgets import QAbstractButton, QApplication, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    WORK = os.path.join(gt.OUT, "save_prompt_keys")
    PROJECT = os.path.join(WORK, "prompt.ntpz")
    Buttons = NatronEngine.Natron.StandardButtonEnum

    def main_windows():
        return [w for w in QApplication.topLevelWidgets()
                if isinstance(w, QMainWindow) and w.isVisible()]

    def modal():
        return QApplication.activeModalWidget()

    def modal_class():
        w = modal()
        return gt.class_name(w) if w is not None else None

    def button_texts(dialog):
        return sorted(b.text() for b in dialog.findChildren(QAbstractButton)
                      if b.isVisible() and gt.class_name(b) != "QCheckBox")

    def press(dialog, key, text):
        target = dialog.focusWidget() or dialog
        QApplication.sendEvent(target, QKeyEvent(QEvent.KeyPress, key, Qt.NoModifier, text))
        QApplication.sendEvent(target, QKeyEvent(QEvent.KeyRelease, key, Qt.NoModifier, text))

    def call_later(fn, result):
        """Runs fn from a timer, so the modal loop it raises does not block this generator."""
        result.clear()

        def fire():
            try:
                result["value"] = fn()
            finally:
                result["done"] = True

        QTimer.singleShot(0, fire)

    def answer(fn, key, text, label, shot_name=None):
        """Raises fn's dialog, presses key in it, and returns (fn's result, the dialog's button texts)."""
        result = {}
        call_later(fn, result)
        ok = yield ("until", lambda: modal() is not None or result.get("done"), DIALOG_TIMEOUT_S)
        require(ok and modal() is not None, label + ": a dialog appeared", "modal is %r" % modal_class())
        dialog = modal()
        texts = button_texts(dialog)
        yield ("sleep", 300)
        if shot_name:
            gt.shot(shot_name)
        press(modal(), key, text)
        ok = yield ("until", lambda: result.get("done"), DIALOG_TIMEOUT_S)
        check(ok, label + ": the key answered the dialog")
        if not ok and modal() is not None:
            modal().reject()
            yield ("until", lambda: result.get("done"), DIALOG_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        leftover = modal_class()
        check(leftover is None, label + ": no dialog is left open (modal is %r)" % leftover)
        return result.get("value"), texts

    def valid_bundle(path):
        try:
            with zipfile.ZipFile(path) as z:
                return any(n.endswith("/project.yaml") for n in z.namelist())
        except (OSError, zipfile.BadZipFile):
            return False

    def node_names():
        return sorted(n.getScriptName() for n in app.getChildren())

    def backdate(path):
        old = time.time() - 3600
        os.utime(path, (old, old))
        return old

    def saved_then_edited():
        """A saved project with an edit the auto-save has seen; returns the file's backdated mtime."""
        if node_names():
            require(app.resetProject(), "reset the project before building the fixture")
            yield ("sleep", SETTLE_MS)
        constant = app.createNode(CONSTANT, -1, None, {})
        require(constant is not None, "create " + CONSTANT, gt.plugin_problem(CONSTANT))
        constant.getParam("color").set(0.2, 0.6, 0.3, 1.0)
        require(app.saveProjectAs(PROJECT), "saved the project to " + PROJECT)
        old = backdate(PROJECT)
        constant.getParam("color").set(0.7, 0.1, 0.1, 1.0)
        yield ("sleep", AUTOSAVE_WAIT_MS)
        return old

    def unchanged(old):
        return abs(os.path.getmtime(PROJECT) - old) < 2

    def test():
        global app
        gt.section("fixtures")
        shutil.rmtree(WORK, ignore_errors=True)
        os.makedirs(WORK)
        NatronEngine.natron.getSettings().getParam("autoSaveDelay").set(AUTOSAVE_DELAY_S)
        old = yield from saved_then_edited()
        nodes = node_names()

        gt.section("cancel")
        closed, texts = yield from answer(app.resetProject, Qt.Key_C, "c", "c at the save prompt",
                                          "save-prompt-keys.png")
        check(texts == sorted(["&Save", "&Discard", "&Cancel"]),
              "the save prompt's buttons read &Save, &Discard, &Cancel (got %r)" % (texts,))
        check(closed is False, "c cancelled the close (resetProject returned %r)" % (closed,))
        check(node_names() == nodes, "c kept the project's nodes (%r)" % (node_names(),))
        check(unchanged(old), "c left the project file alone")

        closed, _ = yield from answer(app.resetProject, Qt.Key_Escape, "\x1b", "Escape at the save prompt")
        check(closed is False, "Escape cancelled the close (resetProject returned %r)" % (closed,))
        check(node_names() == nodes, "Escape kept the project's nodes (%r)" % (node_names(),))
        check(unchanged(old), "Escape left the project file alone")

        gt.section("discard")
        closed, _ = yield from answer(app.resetProject, Qt.Key_D, "d", "d at the save prompt")
        check(closed is True, "d closed the project (resetProject returned %r)" % (closed,))
        ok = yield ("until", lambda: not node_names(), DIALOG_TIMEOUT_S)
        check(ok, "d emptied the graph (%r)" % (node_names(),))
        check(unchanged(old), "d did not write the project file")

        gt.section("save")
        old = yield from saved_then_edited()
        closed, _ = yield from answer(app.resetProject, Qt.Key_S, "s", "s at the save prompt")
        check(closed is True, "s closed the project (resetProject returned %r)" % (closed,))
        ok = yield ("until", lambda: not node_names(), DIALOG_TIMEOUT_S)
        check(ok, "s emptied the graph (%r)" % (node_names(),))
        ok = yield ("until", lambda: os.path.getmtime(PROJECT) > old + 60, DIALOG_TIMEOUT_S)
        check(ok, "s wrote the project file")
        check(valid_bundle(PROJECT), "the written file is a valid bundle")

        gt.section("yes/no")
        guiapp = NatronGui.natron
        require(hasattr(guiapp, "questionDialog"), "NatronGui.natron.questionDialog is bound")
        ask = lambda: guiapp.questionDialog("Question", "Answer from the keyboard?")
        reply, texts = yield from answer(ask, Qt.Key_Y, "y", "y at a Yes/No question")
        check(texts == sorted(["&Yes", "&No"]), "the question's buttons read &Yes, &No (got %r)" % (texts,))
        check(reply == Buttons.eStandardButtonYes, "y answered Yes (got %r)" % (reply,))
        reply, _ = yield from answer(ask, Qt.Key_N, "n", "n at a Yes/No question")
        check(reply == Buttons.eStandardButtonNo, "n answered No (got %r)" % (reply,))

        gt.section("message box")
        # Long text goes to Natron's own message box instead of a QMessageBox.
        long_text = " ".join(["This information is long enough for the scrolling message box."] * 40)
        inform = lambda: guiapp.informationDialog("Information", long_text)
        result = {}
        call_later(inform, result)
        ok = yield ("until", lambda: modal_class() == "MessageBox", DIALOG_TIMEOUT_S)
        require(ok, "the long information dialog is a MessageBox", "modal is %r" % modal_class())
        check(button_texts(modal()) == ["&OK"], "its button reads &OK (got %r)" % (button_texts(modal()),))
        yield ("sleep", 300)
        gt.shot("save-prompt-keys-message-box.png")
        press(modal(), Qt.Key_O, "o")
        ok = yield ("until", lambda: result.get("done"), DIALOG_TIMEOUT_S)
        check(ok, "o closed the message box")
        if not ok and modal() is not None:
            modal().reject()

    def start():
        global app
        app = app1
        gt.run(test())

    def wait_for_window():
        deadline = time.time() + 60.0

        def poll():
            if main_windows():
                QTimer.singleShot(1000, start)
            elif time.time() > deadline:
                check(False, "the main window never came up")
                gt.finish()
            else:
                QTimer.singleShot(100, poll)

        poll()

    wait_for_window()
except Exception:
    traceback.print_exc()
    try:
        from guitest import check as _check, finish as _finish
        _check(False, "script error: " + traceback.format_exc())
        _finish()
    except Exception:
        os._exit(1)
