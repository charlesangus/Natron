"""File > Open refuses a legacy .ntp and a file that is not a project with one error dialog and no
other effect, opens a project folder, and Save on a folder project writes <folder>.ntpz beside it.

The checks, in order:
  refusals on an empty graph   a legacy .ntp and a .png each give exactly one error dialog with the
                               expected text; the instance count, window count, graph and recent
                               files are unchanged
  folder open                  a project folder (a saved .ntpz unzipped) opens in the same window,
                               its nodes come back, and the folder is added to the recent files
  refusals on a loaded graph   the same two refusals with a non-empty graph open no new window
  folder Save                  Cancel at the replace prompt leaves <folder>.ntpz alone, Yes rewrites
                               it, and with no <folder>.ntpz there is no prompt and one is written

Every modal dialog is driven from QTimer callbacks (see run() in guitest.py): the action that
raises it is triggered from a single-shot timer, and the script waits for QApplication's active
modal widget to be the dialog it expects.

Usage (natron-dev container):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        Tests/gui/run-gui-test.sh Tests/gui/m16_open_errors.py <Natron.AppImage>'
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
LOAD_TIMEOUT_S = 60.0
GRADE = "net.sf.openfx.GradePlugin"
BLUR = "net.sf.cimg.CImgBlur"

try:
    import NatronEngine
    from PySide6.QtCore import QTimer
    from PySide6.QtGui import QAction, QImage
    from PySide6.QtCore import QSettings
    from PySide6.QtWidgets import QAbstractButton, QApplication, QLabel, QLineEdit, QMainWindow

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    WORK = os.path.join(gt.OUT, "m16_open_errors")
    BUNDLE = os.path.join(WORK, "fixture.ntpz")
    FOLDER = os.path.join(WORK, "fixture")
    FOLDER_BUNDLE = FOLDER + ".ntpz"
    LEGACY = os.path.join(WORK, "legacy.ntp")
    PNG = os.path.join(WORK, "picture.png")

    LEGACY_TEXT = "Legacy .ntp projects are not supported"

    def main_windows():
        return [w for w in QApplication.topLevelWidgets()
                if isinstance(w, QMainWindow) and w.isVisible()]

    def menu_action(window, text):
        for a in window.findChildren(QAction):
            if a.text().replace("&", "") == text:
                return a
        return None

    def recent_files():
        value = QSettings().value("recentFileList")
        if value is None:
            return []
        if isinstance(value, str):
            return [value] if value else []
        return list(value)

    def graph_snapshot():
        return sorted((n.getScriptName(),) + tuple(n.getPosition()) for n in app.getChildren())

    def state():
        return {
            "instances": NatronEngine.natron.getNumInstances(),
            "windows": len(main_windows()),
            "graph": graph_snapshot(),
            "recent": recent_files(),
        }

    def modal_class():
        w = QApplication.activeModalWidget()
        return gt.class_name(w) if w is not None else None

    def dialog_text(dialog):
        return "\n".join(l.text() for l in dialog.findChildren(QLabel) if l.text())

    def push_button(dialog, *texts):
        for b in dialog.findChildren(QAbstractButton):
            label = b.text().replace("&", "")
            if label in texts:
                return b
        return None

    def trigger_later(action, flag):
        """Raises the action's modal loop from a timer, so this generator keeps running."""
        flag["done"] = False

        def fire():
            try:
                action.trigger()
            finally:
                flag["done"] = True

        QTimer.singleShot(0, fire)

    def submit_open_dialog(path):
        """Types path into the open dialog and presses Open. Returns False if there was no dialog."""
        dialog = QApplication.activeModalWidget()
        if dialog is None or gt.class_name(dialog) != "SequenceFileDialog":
            return False
        edits = [w for w in dialog.findChildren(QLineEdit) if gt.class_name(w) == "FileDialogLineEdit"]
        if not edits:
            return False
        edits[0].setText(path)
        button = push_button(dialog, "Open")
        if button is None:
            return False
        button.click()
        return True

    def open_through_dialog(path, expect_error):
        """File > Open with path typed in. Returns (error text or None, dialog count)."""
        window = main_windows()[0]
        action = menu_action(window, "Open Project...")
        require(action is not None, "found the File > Open Project action")
        flag = {}
        trigger_later(action, flag)
        ok = yield ("until", lambda: modal_class() == "SequenceFileDialog", DIALOG_TIMEOUT_S)
        require(ok, "the open dialog appeared for " + os.path.basename(path), "modal is %r" % modal_class())
        require(submit_open_dialog(path), "typed " + path + " into the open dialog and pressed Open")
        texts = []
        if expect_error:
            ok = yield ("until", lambda: modal_class() == "MessageBox", DIALOG_TIMEOUT_S)
            if ok:
                dialog = QApplication.activeModalWidget()
                texts.append(dialog_text(dialog))
                yield ("sleep", 300)
                gt.shot("m16-open-error-%s.png" % os.path.basename(path).replace(".", "-"))
                button = push_button(dialog, "OK", "Ok")
                require(button is not None, "the error dialog has an OK button")
                button.click()
            else:
                report("NOTE no error dialog appeared; modal is %r" % modal_class())
                if modal_class() is not None:
                    QApplication.activeModalWidget().reject()
        ok = yield ("until", lambda: flag["done"], DIALOG_TIMEOUT_S)
        check(ok, "File > Open returned for " + os.path.basename(path))
        # Any second dialog would show up as a new modal widget once the first is gone.
        yield ("sleep", SETTLE_MS)
        extra = modal_class()
        if extra is not None:
            texts.append(dialog_text(QApplication.activeModalWidget()))
            QApplication.activeModalWidget().reject()
            yield ("sleep", 300)
        return texts, extra

    def expect_refusal(path, needle, label):
        before = state()
        texts, extra = yield from open_through_dialog(path, True)
        after = state()
        check(len(texts) == 1, label + ": exactly one error dialog (got %d: %r)" % (len(texts), texts))
        check(bool(texts) and needle in texts[0],
              label + ": the dialog says %r (got %r)" % (needle, texts[:1]))
        check(extra is None, label + ": no dialog is left open")
        check(after["instances"] == before["instances"],
              label + ": no new app instance (%d -> %d)" % (before["instances"], after["instances"]))
        check(after["windows"] == before["windows"],
              label + ": no new window (%d -> %d)" % (before["windows"], after["windows"]))
        check(after["graph"] == before["graph"], label + ": the graph is unchanged")
        check(after["recent"] == before["recent"],
              label + ": the recent files are unchanged (%r -> %r)" % (before["recent"], after["recent"]))
        check(bool(main_windows()), label + ": the application is still running")

    def save_through_menu(expect_prompt, answer):
        """File > Save Project. Returns the replace prompt's text, or None if none appeared."""
        window = main_windows()[0]
        action = menu_action(window, "Save Project")
        require(action is not None, "found the File > Save Project action")
        flag = {}
        trigger_later(action, flag)
        prompt = None
        if expect_prompt:
            ok = yield ("until", lambda: modal_class() == "MessageBox", DIALOG_TIMEOUT_S)
            check(ok, "the replace prompt appeared (modal is %r)" % modal_class())
            if ok:
                dialog = QApplication.activeModalWidget()
                prompt = dialog_text(dialog)
                gt.shot("m16-save-replace-prompt.png")
                button = push_button(dialog, answer)
                check(button is not None, "the replace prompt has a %s button" % answer)
                if button is not None:
                    button.click()
                else:
                    dialog.reject()
        ok = yield ("until", lambda: flag["done"], DIALOG_TIMEOUT_S)
        check(ok, "File > Save Project returned")
        yield ("sleep", SETTLE_MS)
        leftover = modal_class()
        check(leftover is None, "no dialog is left open after Save (modal is %r)" % leftover)
        if leftover is not None:
            QApplication.activeModalWidget().reject()
        return prompt

    def valid_bundle(path):
        try:
            with zipfile.ZipFile(path) as z:
                return "project.yaml" in z.namelist()
        except (OSError, zipfile.BadZipFile):
            return False

    def test():
        global app
        gt.section("fixtures")
        shutil.rmtree(WORK, ignore_errors=True)
        os.makedirs(WORK)
        with open(LEGACY, "w") as f:
            f.write('<?xml version="1.0" encoding="UTF-8"?>\n<Project><Nodes/></Project>\n')
        image = QImage(4, 4, QImage.Format_RGB32)
        image.fill(0x336699)
        require(image.save(PNG), "wrote " + PNG)

        for pid, x, y in ((GRADE, 100.0, 50.0), (BLUR, 300.0, 150.0)):
            node = app.createNode(pid, -1, None, {})
            require(node is not None, "create " + pid, gt.plugin_problem(pid))
            node.setPosition(x, y)
        saved_nodes = graph_snapshot()
        require(len(saved_nodes) >= 2, "the fixture graph has nodes")
        require(app.saveProjectAs(BUNDLE), "saved the fixture project to " + BUNDLE)
        require(valid_bundle(BUNDLE), "the saved bundle holds project.yaml")
        with zipfile.ZipFile(BUNDLE) as z:
            z.extractall(FOLDER)
        require(os.path.isfile(os.path.join(FOLDER, "project.yaml")), "unzipped the bundle into " + FOLDER)
        # Folder saves land on <folder>.ntpz, which must not exist yet for the no-prompt case.
        os.remove(BUNDLE)

        require(app.resetProject(), "reset the project to an empty graph")
        yield ("sleep", SETTLE_MS)
        report("NOTE empty graph: %r" % (graph_snapshot(),))

        gt.section("refusals, empty graph")
        yield from expect_refusal(LEGACY, LEGACY_TEXT, "legacy .ntp")
        yield from expect_refusal(PNG, "is not a", "png")

        gt.section("folder open")
        before = state()
        texts, extra = yield from open_through_dialog(FOLDER, False)
        check(not texts and extra is None, "opening the folder raised no dialog")
        ok = yield ("until", lambda: graph_snapshot() == saved_nodes, LOAD_TIMEOUT_S)
        check(ok, "the folder's nodes are back (got %r, want %r)" % (graph_snapshot(), saved_nodes))
        after = state()
        check(after["instances"] == before["instances"] and after["windows"] == before["windows"],
              "an empty graph loads the folder in the same window")
        check(FOLDER in after["recent"], "the folder is in the recent files (%r)" % (after["recent"],))
        gt.shot("m16-folder-open.png")

        gt.section("refusals, loaded graph")
        yield from expect_refusal(LEGACY, LEGACY_TEXT, "legacy .ntp")
        yield from expect_refusal(PNG, "is not a", "png")

        gt.section("folder save")
        check(not os.path.exists(FOLDER_BUNDLE), "no " + FOLDER_BUNDLE + " before the first save")
        prompt = yield from save_through_menu(False, None)
        check(prompt is None, "no replace prompt when the .ntpz is not there")
        check(valid_bundle(FOLDER_BUNDLE), "Save wrote %s beside the folder" % FOLDER_BUNDLE)

        old = time.time() - 3600
        os.utime(FOLDER_BUNDLE, (old, old))
        prompt = yield from save_through_menu(True, "Cancel")
        check(prompt is not None and "fixture.ntpz" in prompt, "the replace prompt names fixture.ntpz (got %r)" % (prompt,))
        check(abs(os.path.getmtime(FOLDER_BUNDLE) - old) < 2, "Cancel left the existing .ntpz alone")

        prompt = yield from save_through_menu(True, "Yes")
        check(prompt is not None, "the replace prompt appeared again")
        check(os.path.getmtime(FOLDER_BUNDLE) > old + 60, "Yes rewrote the existing .ntpz")
        check(valid_bundle(FOLDER_BUNDLE), "the rewritten .ntpz is a valid bundle")
        check(os.path.isfile(os.path.join(FOLDER, "project.yaml")), "the project folder is untouched")

    def start():
        global app
        app = app1
        gt.run(test())

    def window_up():
        return bool(main_windows()) and any(menu_action(w, "Open Project...") is not None for w in main_windows())

    def wait_for_window():
        deadline = time.time() + 60.0

        def poll():
            if window_up():
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
