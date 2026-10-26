"""USDRead's File dialog lists USD files: it filters on the USD and Alembic extensions, and the
bare `*` filter lists every file.

The script writes a folder under $NATRON_GUI_TEST_OUT (build/gui-test-out/ by default) holding
scene.usda, notes.txt and README, points a USDRead at scene.usda, opens its settings panel and
calls the File parameter's openFile(). In the dialog that opens it reads the names the file view
lists: scene.usda and not notes.txt under the default filter, then all three once the `*` filter
is applied.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT.
Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0

try:
    from PySide6.QtCore import QMetaObject, Q_ARG, QTimer
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QTreeView

    import guitest as gt
    from guitest import OUT, check, finish, plugin_problem, report, require, run

    folder = os.path.join(OUT, "m20-usd-file-dialog")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    scene_path = os.path.join(folder, "scene.usda")
    with open(scene_path, "w") as f:
        f.write('#usda 1.0\n\ndef Xform "root"\n{\n}\n')
    with open(os.path.join(folder, "notes.txt"), "w") as f:
        f.write("not a scene\n")
    with open(os.path.join(folder, "README"), "w") as f:
        f.write("no extension\n")

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    filename = usd.getParam("filename")
    require(filename is not None, "USDRead has a filename parameter")
    filename.setValue(scene_path)
    usd.setPosition(0, 0)

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return panel
                except RuntimeError:
                    continue
        return None

    def modal_dialog():
        w = QApplication.activeModalWidget()
        return w if w is not None and gt.class_name(w) == "SequenceFileDialog" else None

    def listed_names(dialog):
        views = [v for v in dialog.findChildren(QTreeView) if gt.class_name(v) == "SequenceDialogView"]
        if not views:
            return []
        view = views[0]
        model = view.model()
        root = view.rootIndex()
        return [str(model.index(r, 0, root).data()) for r in range(model.rowCount(root))]

    def lists(dialog, name):
        return any(n == name or n.endswith("/" + name) for n in listed_names(dialog))

    def steps():
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        app.selectNode(usd, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(usd) is not None, POLL_TIMEOUT_S)
        require(ok, "open the USDRead's settings panel")

        opened = {}

        def open_dialog():
            # The dialog runs its own modal loop, so it is raised from a timer.
            try:
                filename.openFile()
            finally:
                opened["returned"] = True

        QTimer.singleShot(0, open_dialog)
        ok = yield ("until", lambda: modal_dialog() is not None, POLL_TIMEOUT_S)
        require(ok, "the File parameter opens a file dialog")
        dialog = modal_dialog()
        filters = [e.text() for e in dialog.findChildren(QLineEdit) if "*" in e.text()]
        report("NOTE dialog filter: %r" % (filters,))
        check(any("*.usda" in t for t in filters), "the dialog filters on USD extensions (%r)" % (filters,))

        ok = yield ("until", lambda: lists(dialog, "scene.usda"), POLL_TIMEOUT_S)
        report("NOTE listed under the default filter: %r" % (listed_names(dialog),))
        check(ok, "the dialog lists scene.usda")
        check(not lists(dialog, "notes.txt"), "the USD filter hides notes.txt")
        gt.shot("m20-usd-file-dialog.png")

        QMetaObject.invokeMethod(dialog, "applyFilter", Q_ARG(str, "*"))
        ok = yield ("until", lambda: lists(dialog, "notes.txt") and lists(dialog, "README") and lists(dialog, "scene.usda"),
                    POLL_TIMEOUT_S)
        report("NOTE listed under `*`: %r" % (listed_names(dialog),))
        check(ok, "the `*` filter lists every file")
        gt.shot("m20-usd-file-dialog-all.png")

        dialog.reject()
        ok = yield ("until", lambda: opened.get("returned", False), POLL_TIMEOUT_S)
        check(ok, "the dialog closes")
        yield ("sleep", SETTLE_MS)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
