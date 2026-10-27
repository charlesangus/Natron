"""GUI test for the AI Models page of the Preferences window: the model table, the licence notice
before a download, installing the fixture model, the default-model radio and deleting the model.
Nothing is fetched from the network: the manifest is the built-in one plus two entries whose url is
a file:// link to Tests/fixtures/ai/identity.dtmodel, and models install under the output folder.

Screenshots, in $NATRON_GUI_TEST_OUT (default build/gui-test-out/):
    m204-ai-models.png            the page before the download
    m204-ai-models-licence.png    the licence notice of a model with research-only training data
    m204-ai-models-installed.png  the page once the fixture is installed

Usage, in the natron-dev container:
    Tests/gui/run-gui-test.sh Tests/gui/m204_ai_models.py <Natron.AppImage or release binary>
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
SETTLE_MS = 400

FIXTURE_PACKAGE = "test-identity"
FIXTURE_SHA256 = "eb95dd0d3cfecc9a713799210f0f9fdcb29b1745a8adbf846ddc1b7ad20a92b8"
FIXTURE_SIZE = 602
# Two models of one package: installing one installs both.
FIXTURE_DENOISE = "test-identity"
FIXTURE_RESEARCH = "test-identity-research"

try:
    import guitest as gt

    # The manifest has to be in place before anything reads the model list, which the AI Models
    # page does first when the Preferences window is built.
    work = os.path.join(gt.OUT, "m204-ai")
    data_home = os.path.join(work, "data")
    os.makedirs(data_home, exist_ok=True)
    with open(os.path.join(gt.ROOT, "Engine", "Ai", "data", "ai_models.json")) as f:
        manifest = json.load(f)
    archive_url = "file://" + os.path.join(gt.FIXTURES, "ai", "identity.dtmodel")

    def fixture_entry(model_id, task, name, osaid, training):
        return {
            "id": model_id, "task": task, "name": name, "package": FIXTURE_PACKAGE, "files": ["model.onnx"],
            "url": archive_url, "sha256": FIXTURE_SHA256, "size": FIXTURE_SIZE, "weightsLicence": "MIT",
            "trainingDataLicence": training, "osaid": osaid, "cardUrl": "https://example.invalid/card",
            "default": False,
        }

    manifest["models"].append(fixture_entry(FIXTURE_DENOISE, "denoise", "Test Identity", "Open Source AI", "none"))
    manifest["models"].append(fixture_entry(FIXTURE_RESEARCH, "mask-object", "Test Identity Research", "Open Weights",
                                            "SA-1B: custom Meta research-only license"))
    manifest_path = os.path.join(work, "manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=1)
    os.environ["NATRON_AI_MANIFEST"] = manifest_path
    os.environ["XDG_DATA_HOME"] = data_home

    from PySide6.QtCore import QMetaObject, QRect, QTimer, Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import (QAbstractButton, QApplication, QMessageBox, QRadioButton,
                                   QTableWidget, QTreeWidget)

    import NatronEngine
    from guitest import check, report, require

    def gui_widget():
        found = gt.widgets_of("Gui")
        return found[0] if found else None

    def node_graph_up():
        return any(v.isVisible() for v in gt.widgets_of("NodeGraph"))

    def open_prefs():
        for w in gt.widgets_of("PreferencesPanel"):
            if w.isVisible():
                return w
        return None

    def select_page(prefs, name):
        for tree in prefs.findChildren(QTreeWidget):
            items = tree.findItems(name, Qt.MatchFlag.MatchExactly | Qt.MatchFlag.MatchRecursive, 0)
            if items:
                tree.setCurrentItem(items[0])
                return True
        return False

    def page_tree_labels(prefs):
        out = []
        for tree in prefs.findChildren(QTreeWidget):
            for t in range(tree.topLevelItemCount()):
                out.append(tree.topLevelItem(t).text(0))
        return out

    def table(prefs):
        for w in prefs.findChildren(QTableWidget, "aiModelsTable"):
            if w.isVisible():
                return w
        return None

    def button(prefs, name):
        found = prefs.findChildren(QAbstractButton, name)
        return found[0] if found else None

    def row_of(tbl, name):
        for r in range(tbl.rowCount()):
            if tbl.item(r, 0) is not None and tbl.item(r, 0).text() == name:
                return r
        return -1

    def status(tbl, name):
        r = row_of(tbl, name)
        return tbl.item(r, 5).text() if r >= 0 else None

    def modal(object_name):
        w = QApplication.activeModalWidget()
        return w if (w is not None and w.objectName() == object_name) else None

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

    def installed_dirs():
        found = []
        for dirpath, dirnames, _ in os.walk(data_home):
            if FIXTURE_PACKAGE in dirnames and os.path.basename(dirpath) == "models":
                found.append(os.path.join(dirpath, FIXTURE_PACKAGE))
        return found

    def steps():
        gt.section("ai-models")
        settings = NatronEngine.natron.getSettings()
        defaults_param = settings.getParam("aiDefaultModels")
        require(defaults_param is not None, "Settings has a parameter named aiDefaultModels")

        ok = yield ("until", lambda: node_graph_up() and gui_widget() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window and node graph are on screen")

        QMetaObject.invokeMethod(gui_widget(), "showSettings")
        ok = yield ("until", lambda: open_prefs() is not None, POLL_TIMEOUT_S)
        prefs = open_prefs()
        require(ok and prefs is not None, "the Preferences window opened (slot Gui.showSettings)")
        prefs.raise_()
        yield ("sleep", SETTLE_MS)

        labels = page_tree_labels(prefs)
        report("NOTE top-level pages: %r" % (labels,))
        require("AI Models" in labels, "the page tree has an \"AI Models\" item")
        check("AI" in labels and labels.index("AI Models") == labels.index("AI") + 1,
              "\"AI Models\" comes right after \"AI\" in the page tree")

        require(select_page(prefs, "AI Models"), "selected the \"AI Models\" page")
        ok = yield ("until", lambda: table(prefs) is not None, POLL_TIMEOUT_S)
        tbl = table(prefs)
        require(ok and tbl is not None, "the AI Models page shows its table (QTableWidget aiModelsTable)")

        headers = [tbl.horizontalHeaderItem(c).text() for c in range(tbl.columnCount())]
        check(headers == ["Name", "Task", "Size", "Weights Licence", "Training-Data Licence", "Status", "Default"],
              "the table columns are Name, Task, Size, Weights Licence, Training-Data Licence, Status, Default (got %r)" % (headers,))
        check(tbl.rowCount() == len(manifest["models"]),
              "the table has one row per manifest model (%d, got %d)" % (len(manifest["models"]), tbl.rowCount()))
        check(row_of(tbl, "SegNext ViT-B SAx2 HQ") >= 0, "the built-in SegNext model is listed")
        check(status(tbl, "Test Identity") == "Missing", "the fixture model reads Missing (got %r)" % status(tbl, "Test Identity"))

        check(prefs.frameGeometry().width() <= 1920 and prefs.frameGeometry().height() <= 1080,
              "the Preferences window fits 1920x1080 (got %dx%d)" % (prefs.frameGeometry().width(), prefs.frameGeometry().height()))
        check(tbl.horizontalScrollBar().maximum() == 0, "the table needs no horizontal scrolling")
        screen_crop("m204-ai-models.png", prefs)

        research_row = row_of(tbl, "Test Identity Research")
        require(research_row >= 0, "the research-only fixture model is listed")
        tbl.selectRow(research_row)
        yield ("sleep", SETTLE_MS)
        download = button(prefs, "aiModelsDownload")
        require(download is not None and download.isEnabled(), "the Download button is enabled for a missing model")

        # The notice is modal: click from the event loop so that this generator is not blocked.
        QTimer.singleShot(0, download.click)
        ok = yield ("until", lambda: modal("aiModelLicenceNotice") is not None, POLL_TIMEOUT_S)
        notice = modal("aiModelLicenceNotice")
        require(ok and notice is not None, "Download shows the licence notice (QMessageBox aiModelLicenceNotice)")
        text = notice.text()
        check("research use only" in text, "the notice warns that the training data is for research use only")
        check("https://example.invalid/card" in text, "the notice links the model card")
        check("MIT" in text and "Open Weights" in text, "the notice names the weights licence and the OSAID class")
        yield ("sleep", SETTLE_MS)
        screen_crop("m204-ai-models-licence.png", notice)
        notice.button(QMessageBox.StandardButton.Yes).click()

        ok = yield ("until", lambda: status(tbl, "Test Identity Research") == "Installed"
                    and status(tbl, "Test Identity") == "Installed", POLL_TIMEOUT_S)
        check(ok, "both models of the fixture package read Installed (got %r and %r)"
              % (status(tbl, "Test Identity Research"), status(tbl, "Test Identity")))
        report("NOTE installed under %r" % (installed_dirs(),))
        check(len(installed_dirs()) == 1, "the package installed under the test's XDG_DATA_HOME")
        check(not download.isEnabled(), "the Download button is disabled for an installed model")
        delete = button(prefs, "aiModelsDelete")
        check(delete is not None and delete.isEnabled(), "the Delete button is enabled for an installed model")
        screen_crop("m204-ai-models-installed.png", prefs)

        radio = prefs.findChildren(QRadioButton, "aiModelDefault_" + FIXTURE_DENOISE)
        require(len(radio) == 1, "the fixture model has a Default radio button")
        check(not radio[0].isChecked(), "the fixture model is not the Denoise default yet")
        radio[0].click()
        yield ("sleep", SETTLE_MS)
        stored = json.loads(defaults_param.getValue() or "{}")
        check(stored.get("denoise") == FIXTURE_DENOISE,
              "picking Default stores the model in aiDefaultModels (got %r)" % (stored,))

        QTimer.singleShot(0, delete.click)
        ok = yield ("until", lambda: modal("aiModelDeleteConfirm") is not None, POLL_TIMEOUT_S)
        confirm = modal("aiModelDeleteConfirm")
        require(ok and confirm is not None, "Delete asks for confirmation (QMessageBox aiModelDeleteConfirm)")
        check("Test Identity" in confirm.text(), "the confirmation names the models of the package")
        confirm.button(QMessageBox.StandardButton.Yes).click()
        ok = yield ("until", lambda: status(tbl, "Test Identity Research") == "Missing"
                    and status(tbl, "Test Identity") == "Missing", POLL_TIMEOUT_S)
        check(ok, "both models read Missing after Delete")
        check(not installed_dirs(), "Delete removed the package folder")

        discard = [b for b in prefs.findChildren(QAbstractButton) if b.isVisible() and b.text() == "Discard"]
        require(len(discard) == 1, "the Preferences window has one visible \"Discard\" button")
        discard[0].click()
        ok = yield ("until", lambda: open_prefs() is None, POLL_TIMEOUT_S)
        check(ok, "the Preferences window closed after Discard")
        stored = json.loads(defaults_param.getValue() or "{}")
        check("denoise" not in stored, "Discard reverted the default model pick (got %r)" % (stored,))

    gt.run(steps())
except Exception:
    import traceback
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + traceback.format_exc() + "\n")
    os._exit(1)
