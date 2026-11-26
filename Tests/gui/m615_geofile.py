"""GeoFile's File dialog is the sequence-aware one, filtered on .ngeo, and GeoFileCache's save
dialog defaults to .ngeo.

The script writes a folder under $NATRON_GUI_TEST_OUT holding shape.0001.ngeo to
shape.0005.ngeo and notes.txt, points a GeoFile at notes.txt, opens its settings panel and calls
the File parameter's openFile(). In the dialog it checks:
  - filter: the filter reads *.ngeo, and notes.txt is not listed;
  - sequence: the five files are one item, shape.####.ngeo; opening it sets the File to the
    pattern and First Frame and Last Frame to 1 and 5, and GeoFile has every Read frame-handling
    parameter;
  - cache: a GeoFileCache's Filename save dialog, opened from its button, shows the ngeo extension.
Screenshots: m615_geofile_dialog.png and m615_geofile_cache_dialog.png in $NATRON_GUI_TEST_OUT.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m615_geofile.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0
FRAMES = range(1, 6)
USER_FRIENDLY_NAME_ROLE = 0x0100 + 3
FRAME_KNOBS = ("firstFrame", "beforeFirstFrame", "lastFrame", "afterLastFrame", "onMissingFrame",
               "frameOffsetMode", "startAt", "frameOffset")

try:
    from PySide6.QtCore import QItemSelectionModel, QMetaObject, QTimer
    from PySide6.QtWidgets import QApplication, QCheckBox, QComboBox, QLabel, QLineEdit, QPushButton, QTreeView

    import guitest as gt
    from guitest import OUT, check, finish, plugin_problem, report, require, run

    folder = os.path.join(OUT, "m615-geofile")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    for name in os.listdir(folder):
        os.remove(os.path.join(folder, name))
    for frame in FRAMES:
        with open(os.path.join(folder, "shape.%04d.ngeo" % frame), "w") as f:
            f.write("placeholder\n")
    notes = os.path.join(folder, "notes.txt")
    with open(notes, "w") as f:
        f.write("not geometry\n")

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    geo = app.createNode("GeoFile")
    require(geo is not None, "create GeoFile", plugin_problem("GeoFile"))
    geo.setPosition(0, 0)
    file_param = hold(geo.getParam("file"))
    require(file_param is not None, "GeoFile has a file parameter")
    file_param.setValue(notes)
    cache = app.createNode("GeoFileCache")
    require(cache is not None, "create GeoFileCache", plugin_problem("GeoFileCache"))
    cache.setPosition(300, 0)
    cache_file = hold(cache.getParam("filename"))
    require(cache_file is not None, "GeoFileCache has a filename parameter")

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return hold(views[0]) if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return hold(panel)
                except RuntimeError:
                    continue
        return None

    def modal_dialog():
        w = QApplication.activeModalWidget()
        return hold(w) if w is not None and gt.class_name(w) == "SequenceFileDialog" else None

    def file_view(dialog):
        # SequenceDialogView has no Q_OBJECT, so it shows up as the dialog's only visible QTreeView.
        views = [v for v in dialog.findChildren(QTreeView) if v.isVisible()]
        return hold(views[0]) if views else None

    def items(dialog):
        view = file_view(dialog)
        if view is None:
            return []
        model = view.model()
        root = view.rootIndex()
        listed = []
        for r in range(model.rowCount(root)):
            index = model.index(r, 0, root)
            listed.append((str(index.data(USER_FRIENDLY_NAME_ROLE) or index.data() or ""), index))
        return listed

    def names(dialog):
        return [n for n, _ in items(dialog)]

    def dialog_texts(dialog):
        texts = [e.text() for e in dialog.findChildren(QLineEdit)]
        for combo in dialog.findChildren(QComboBox):
            texts.append(combo.currentText())
            texts.extend(combo.itemText(i) for i in range(combo.count()))
        return texts

    opened = {}

    def open_dialog(param):
        # The dialog runs its own modal loop, so it is raised from a timer.
        opened["returned"] = False

        def go():
            try:
                param.openFile()
            finally:
                opened["returned"] = True

        QTimer.singleShot(0, go)

    def file_button(panel, script_name):
        """The open-file button of the knob named script_name, named <script_name>OpenFileButton."""
        if panel is None:
            return None
        name = script_name + "OpenFileButton"
        for w in panel.findChildren(QPushButton):
            try:
                if w.objectName() == name and w.isVisible():
                    return hold(w)
            except RuntimeError:
                continue
        return None

    def click_file_button(button):
        # OutputFileParam.openFile() only opens user knobs' dialogs, so the button is clicked.
        opened["returned"] = False

        def go():
            try:
                button.click()
            finally:
                opened["returned"] = True

        QTimer.singleShot(0, go)

    def open_panel(node):
        app.clearSelection()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        return ok

    def steps():
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        ok = yield from open_panel(geo)
        require(ok, "open the GeoFile's settings panel")
        for name in FRAME_KNOBS:
            check(geo.getParam(name) is not None, "GeoFile has a %s parameter" % name)

        gt.section("filter")
        open_dialog(file_param)
        ok = yield ("until", lambda: modal_dialog() is not None, POLL_TIMEOUT_S)
        require(ok, "the File parameter opens a sequence file dialog")
        dialog = modal_dialog()
        filters = [e.text() for e in dialog.findChildren(QLineEdit) if "*" in e.text()]
        report("NOTE dialog filter: %r" % (filters,))
        check("*.ngeo" in filters, "the dialog filters on *.ngeo (%r)" % (filters,))
        sequences = [w for w in dialog.findChildren(QCheckBox) if w.objectName() == "SequenceFileDialogSequencesCheckBox"]
        check(len(sequences) == 1 and sequences[0].isVisible(), "the dialog is a sequence dialog")

        gt.section("sequence")
        ok = yield ("until", lambda: any(n.startswith("shape.") for n in names(dialog)), POLL_TIMEOUT_S)
        listed = names(dialog)
        report("NOTE listed: %r" % (listed,))
        require(ok, "the dialog lists the shape files")
        shapes = [n for n in listed if n.startswith("shape.")]
        check(len(shapes) == 1 and shapes[0].startswith("shape.####.ngeo"),
              "the five frames are one item, shape.####.ngeo (got %r)" % (shapes,))
        check(not any("notes.txt" in n for n in listed), "the ngeo filter hides notes.txt")
        yield ("sleep", SETTLE_MS)
        gt.shot("m615_geofile_dialog.png")
        index = next(i for n, i in items(dialog) if n.startswith("shape."))
        file_view(dialog).selectionModel().select(index, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        yield ("sleep", SETTLE_MS)
        QMetaObject.invokeMethod(dialog, "openSelectedFiles")
        ok = yield ("until", lambda: opened.get("returned", False), POLL_TIMEOUT_S)
        require(ok, "opening the sequence closes the dialog")
        value = file_param.getValue()
        check(value.endswith("shape.####.ngeo"), "the File is the sequence's pattern (got %r)" % value)
        ok = yield ("until", lambda: (geo.getParam("firstFrame").getValue(), geo.getParam("lastFrame").getValue()) == (1, 5),
                    POLL_TIMEOUT_S)
        check(ok, "First Frame and Last Frame are 1 and 5 (got %r, %r)"
              % (geo.getParam("firstFrame").getValue(), geo.getParam("lastFrame").getValue()))

        gt.section("cache")
        ok = yield from open_panel(cache)
        require(ok, "open the GeoFileCache's settings panel")
        ok = yield ("until", lambda: file_button(panel_of(cache), "filename") is not None, POLL_TIMEOUT_S)
        if not ok:
            panel = panel_of(cache)
            report("NOTE buttons in the panel: %r" % ([(b.objectName(), b.isVisible()) for b in panel.findChildren(QPushButton)]
                                                     if panel is not None else None,))
        require(ok, "found the Filename parameter's open-file button")
        button = file_button(panel_of(cache), "filename")
        click_file_button(button)
        ok = yield ("until", lambda: modal_dialog() is not None, POLL_TIMEOUT_S)
        require(ok, "the Filename parameter opens a save dialog")
        dialog = modal_dialog()
        texts = dialog_texts(dialog)
        report("NOTE save dialog texts: %r" % (texts,))
        check(any("ngeo" in t for t in texts), "the save dialog shows the ngeo extension")
        check(not any(t.strip() in ("*.exr", "exr") for t in texts), "the save dialog offers no image formats")
        yield ("sleep", SETTLE_MS)
        gt.shot("m615_geofile_cache_dialog.png")
        dialog.reject()
        ok = yield ("until", lambda: opened.get("returned", False), POLL_TIMEOUT_S)
        check(ok, "the save dialog closes")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
