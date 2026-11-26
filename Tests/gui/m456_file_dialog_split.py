import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0

NEW_FOLDER = "m456 new folder"
USER_FRIENDLY_NAME_ROLE = 0x0100 + 3
CHUNK_RANGE_ROLE = 0x0100 + 4

try:
    from PySide6.QtCore import QItemSelectionModel, QMetaObject, QPoint, Q_ARG, QTimer
    from PySide6.QtWidgets import (QApplication, QCheckBox, QFrame, QInputDialog, QLabel, QLineEdit, QMenu,
                                   QTreeView, QWidget)

    import guitest as gt
    from guitest import OUT, check, finish, plugin_problem, report, require, run

    folder = os.path.join(OUT, "m456-file-dialog-split")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    for name in os.listdir(folder):
        path = os.path.join(folder, name)
        if os.path.isdir(path):
            os.rmdir(path)
        else:
            os.remove(path)
    for frame in list(range(10, 21)) + list(range(30, 41)):
        open(os.path.join(folder, "plate.%04d.exr" % frame), "w").close()

    read = app.createReader(os.path.join(folder, "plate.0010.exr"))
    require(read is not None, "create a Read", plugin_problem("Read"))
    filename = read.getParam("filename")
    require(filename is not None, "the Read has a filename parameter")
    read.setPosition(0, 0)

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

    def file_view(dialog):
        # SequenceDialogView has no Q_OBJECT, so it shows up as the dialog's only visible QTreeView.
        views = [v for v in dialog.findChildren(QTreeView) if v.isVisible()]
        return views[0] if views else None

    def items(dialog):
        view = file_view(dialog)
        if view is None:
            return []
        model = view.model()
        root = view.rootIndex()
        listed = []
        for r in range(model.rowCount(root)):
            index = model.index(r, 0, root)
            name = str(index.data(USER_FRIENDLY_NAME_ROLE) or "")
            if name.startswith("plate."):
                chunk = index.data(CHUNK_RANGE_ROLE)
                listed.append((name, tuple(int(f) for f in chunk) if chunk else None, index))
        return listed

    def names(dialog):
        return [n for n, _, _ in items(dialog)]

    def child(dialog, cls, name):
        return dialog.findChild(cls, name)

    opened = {}

    def open_dialog():
        # The dialog runs its own modal loop, so it is raised from a timer.
        opened["returned"] = False

        def go():
            try:
                filename.openFile()
            finally:
                opened["returned"] = True

        QTimer.singleShot(0, go)

    def check_rows(dialog):
        row1 = ["SequenceFileDialogPreviousButton", "SequenceFileDialogNextButton", "SequenceFileDialogUpButton",
                "SequenceFileDialogHomeButton", "separator", "SequenceFileDialogLookInCombo",
                "SequenceFileDialogNewFolderButton"]
        back = child(dialog, QWidget, row1[0])
        require(back is not None, "the dialog has a back button")
        row = back.parentWidget()
        require(row is not None and row.layout() is not None, "the first row has a layout")
        order = []
        for i in range(row.layout().count()):
            w = row.layout().itemAt(i).widget()
            if w is None:
                order.append("stretch")
            elif isinstance(w, QFrame) and w.frameShape() == QFrame.VLine:
                order.append("separator")
            else:
                order.append(w.objectName())
        report("NOTE first row: %r" % (order,))
        check(order == row1, "the first row runs back, forward, up, home, |, path, New Folder")
        check(child(dialog, QWidget, "SequenceFileDialogListViewButton") is None
              and child(dialog, QWidget, "SequenceFileDialogDetailViewButton") is None,
              "the dialog has no list and details toggles")
        view = file_view(dialog)
        require(view is not None, "the dialog shows its file view")
        check(all(not view.isColumnHidden(c) for c in (1, 2, 3)),
              "the file view shows the size, type and date columns")
        combo = child(dialog, QWidget, "SequenceFileDialogLookInCombo")
        new_folder = child(dialog, QWidget, "SequenceFileDialogNewFolderButton")
        check(combo is not None and combo.width() > 4 * back.width(), "the path stretches across the row")
        check(new_folder is not None and new_folder.text() == "" and not new_folder.icon().isNull(),
              "New Folder is an icon button with no text")
        check(new_folder is not None and new_folder.width() == back.width(), "New Folder is as wide as the back button")
        check(new_folder is not None and new_folder.isEnabled(), "New Folder is enabled in an open dialog")

        sequences = child(dialog, QCheckBox, "SequenceFileDialogSequencesCheckBox")
        split = child(dialog, QCheckBox, "SequenceFileDialogSplitOnGapsCheckBox")
        hidden = child(dialog, QCheckBox, "SequenceFileDialogShowHiddenCheckBox")
        require(sequences is not None and split is not None and hidden is not None, "the second row has its three checkboxes")
        check(sequences.text() == "Sequences" and split.text() == "Split on gaps" and hidden.text() == "Show hidden",
              "the checkboxes read Sequences, Split on gaps and Show hidden")
        y1 = back.mapTo(dialog, QPoint(0, 0)).y()
        pos = [w.mapTo(dialog, QPoint(0, 0)) for w in (sequences, split, hidden)]
        check(all(p.y() > y1 for p in pos) and pos[0].y() == pos[1].y() == pos[2].y(), "the checkboxes sit on a second row")
        check(pos[0].x() < pos[1].x() < pos[2].x(), "the second row reads Sequences, Split on gaps, Show hidden")
        right = hidden.mapTo(dialog, QPoint(hidden.width(), 0)).x()
        check(right > dialog.width() - 40, "Show hidden is right-aligned (right edge %d of %d)" % (right, dialog.width()))
        check(split.isEnabled(), "Split on gaps is enabled while Sequences is checked")
        sequences.setChecked(False)
        check(not split.isEnabled(), "Split on gaps is disabled while Sequences is unchecked")
        sequences.setChecked(True)

    def close(dialog):
        dialog.reject()
        return ("until", lambda: opened.get("returned", False), POLL_TIMEOUT_S)

    def steps():
        ok = yield ("until", lambda: any(w.isVisible() for w in gt.widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "the node graph is up")
        view = node_graph_view()
        require(view is not None, "the node graph is on screen")
        app.selectNode(read, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(read) is not None, POLL_TIMEOUT_S)
        require(ok, "open the Read's settings panel")

        gt.section("toolbar")
        open_dialog()
        ok = yield ("until", lambda: modal_dialog() is not None, POLL_TIMEOUT_S)
        require(ok, "the File parameter opens a file dialog")
        dialog = modal_dialog()
        check(dialog.width() <= 1920 and dialog.height() <= 1080, "the dialog fits 1920x1080 (%dx%d)" % (dialog.width(), dialog.height()))
        check_rows(dialog)
        split = child(dialog, QCheckBox, "SequenceFileDialogSplitOnGapsCheckBox")
        if split.isChecked():
            split.setChecked(False)

        gt.section("joined")
        ok = yield ("until", lambda: len(items(dialog)) == 1, POLL_TIMEOUT_S)
        report("NOTE listed joined: %r" % (names(dialog),))
        check(ok and items(dialog)[0][1] is None, "the sequence with a gap is one item")
        gt.shot("m456-dialog-joined.png")

        gt.section("split")
        split.setChecked(True)
        ok = yield ("until", lambda: len(items(dialog)) == 2, POLL_TIMEOUT_S)
        listed = items(dialog)
        report("NOTE listed split: %r" % (names(dialog),))
        check(ok, "Split on gaps lists two items")
        check([(n, c) for n, c, _ in listed] == [("plate.####.exr 10-20", (10, 20)), ("plate.####.exr 30-40", (30, 40))],
              "the items are plate.####.exr 10-20 and 30-40")
        gt.shot("m456-dialog-split.png")
        # A hidden column must not come back from the saved state on the next open.
        file_view(dialog).setColumnHidden(2, True)
        yield close(dialog)

        gt.section("persistence")
        open_dialog()
        ok = yield ("until", lambda: modal_dialog() is not None, POLL_TIMEOUT_S)
        require(ok, "the dialog opens again")
        dialog = modal_dialog()
        split = child(dialog, QCheckBox, "SequenceFileDialogSplitOnGapsCheckBox")
        check(split.isChecked(), "Split on gaps is still checked on reopen")
        check(all(not file_view(dialog).isColumnHidden(c) for c in (1, 2, 3)),
              "the reopened dialog shows every detail column though one was hidden when it closed")

        gt.section("open a run")
        ok = yield ("until", lambda: len(items(dialog)) == 2, POLL_TIMEOUT_S)
        require(ok, "the reopened dialog lists the two runs")
        late = [index for _, chunk, index in items(dialog) if chunk == (30, 40)]
        require(len(late) == 1, "the 30-40 item is listed")
        file_view(dialog).selectionModel().select(late[0], QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        yield ("sleep", SETTLE_MS)
        QMetaObject.invokeMethod(dialog, "openSelectedFiles")
        ok = yield ("until", lambda: opened.get("returned", False), POLL_TIMEOUT_S)
        require(ok, "opening the 30-40 item closes the dialog")
        yield ("sleep", SETTLE_MS)
        value = filename.getValue()
        first = read.getParam("firstFrame").getValue()
        last = read.getParam("lastFrame").getValue()
        report("NOTE Read file %r, frames %d-%d" % (value, first, last))
        check(value.endswith("plate.####.exr"), "the Read loads the sequence's pattern")
        check((first, last) == (30, 40), "the Read's first and last frame are 30 and 40")

        gt.section("context menu")
        open_dialog()
        ok = yield ("until", lambda: modal_dialog() is not None, POLL_TIMEOUT_S)
        require(ok, "the dialog opens a third time")
        dialog = modal_dialog()
        split = child(dialog, QCheckBox, "SequenceFileDialogSplitOnGapsCheckBox")
        split.setChecked(False)
        ok = yield ("until", lambda: len(items(dialog)) == 1, POLL_TIMEOUT_S)
        require(ok, "with Split on gaps unchecked the sequence is one item")
        tree = file_view(dialog)
        at = tree.visualRect(items(dialog)[0][2]).center()
        QTimer.singleShot(0, lambda: QMetaObject.invokeMethod(dialog, "showContextMenu", Q_ARG(QPoint, at)))
        ok = yield ("until", lambda: isinstance(QApplication.activePopupWidget(), QMenu), POLL_TIMEOUT_S)
        require(ok, "right-clicking the sequence opens a menu")
        menu = QApplication.activePopupWidget()
        texts = [a.text() for a in menu.actions() if a.text()]
        report("NOTE context menu: %r" % (texts,))
        actions = [a for a in menu.actions() if a.text() == "Split sequence"]
        check(len(actions) == 1, "the menu offers Split sequence")
        if actions:
            actions[0].trigger()
        menu.close()
        ok = yield ("until", lambda: len(items(dialog)) == 2, POLL_TIMEOUT_S)
        check(ok, "Split sequence lists the sequence's two runs")
        check(not split.isChecked(), "splitting one sequence leaves Split on gaps unchecked")

        gt.section("new folder")
        require(child(dialog, QWidget, "SequenceFileDialogNewFolderButton") is not None,
                "the dialog has a New Folder button")

        def click_new_folder():
            # The prompt runs its own modal loop, so the button is clicked from a timer.
            d = modal_dialog()
            if d is not None:
                d.findChild(QWidget, "SequenceFileDialogNewFolderButton").click()
        QTimer.singleShot(0, click_new_folder)

        def prompt():
            w = QApplication.activeModalWidget()
            return w if isinstance(w, QInputDialog) and w.objectName() == "SequenceFileDialogNewFolderPrompt" else None
        ok = yield ("until", lambda: prompt() is not None, POLL_TIMEOUT_S)
        require(ok, "New Folder asks for the folder's name")
        prompt().setTextValue(NEW_FOLDER)
        prompt().accept()
        created = os.path.join(folder, NEW_FOLDER)
        ok = yield ("until", lambda: os.path.isdir(created), POLL_TIMEOUT_S)
        check(ok, "New Folder creates %s" % created)

        def listed_names():
            dialog = modal_dialog()
            view = file_view(dialog) if dialog is not None else None
            if view is None:
                return []
            model, root = view.model(), view.rootIndex()
            return [str(model.index(r, 0, root).data() or "") for r in range(model.rowCount(root))]

        def selected_names():
            dialog = modal_dialog()
            view = file_view(dialog) if dialog is not None else None
            if view is None:
                return []
            return [str(i.data() or "") for i in view.selectionModel().selectedRows()]
        ok = yield ("until", lambda: NEW_FOLDER in listed_names(), POLL_TIMEOUT_S)
        check(ok, "the current folder lists the new folder (%r)" % (listed_names(),))
        root_name = str(file_view(modal_dialog()).rootIndex().data() or "") if modal_dialog() is not None else ""
        check(root_name == os.path.basename(folder), "the dialog stays in the current folder (%r)" % (root_name,))
        ok = yield ("until", lambda: selected_names() == [NEW_FOLDER], POLL_TIMEOUT_S)
        check(ok, "the new folder is the selection (%r)" % (selected_names(),))
        gt.shot("m456-dialog-new-folder.png")
        dialog = modal_dialog()
        require(dialog is not None, "the dialog is still open")
        yield close(dialog)
        yield ("sleep", SETTLE_MS)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
