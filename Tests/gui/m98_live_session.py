"""A two-process GUI test of a live session: one Natron hosts its project on loopback through File >
Host Live Session..., a second Natron joins it through File > Join Live Session..., and edits made
in either window must reach the other. After every step both sides' projects must be identical:
each side writes a bundle with app.saveTempProject() and every member but the manifest is compared
(the current frame aside, which each side keeps for itself). It is driven by
Tests/gui/run-gui-test.sh, not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    NATRON_GUI_TEST_OUT=build/m98-live/out GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/m98_live_session.py <AppImage or build/release/App/Natron>

The harness starts one Natron, the host. The host starts the client itself, with the Natron the
harness ran (NATRON_GUI_TEST_BINARY), on the same display, with M98_ROLE=client and its results in
<out>/client/. The two talk through files in <out>/sync/: the host writes cmd-<n>, the client
carries it out and answers in ack-<n>. The client's own checks are copied into the host's results.

On the client: a drag of the Grade's mix slider, a Set Key from the Blur's size menu, a node drag
in the graph, a node created with its panel open and connected, a paint stroke drawn in the viewer
and a user parameter. On the host: a rename typed into the panel's name field and a node deleted
with Backspace in the graph. Then the host stops hosting, and the client must keep an unsaved copy
it can still edit. The connection and the user parameter are made with Python calls, as no
gesture-level helper exists for them.
"""
import os
import re
import subprocess
import sys
import time
import traceback
import zipfile

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

ROLE = os.environ.get("M98_ROLE", "host")
STARTUP_TIMEOUT_S = 90.0
WATCHDOG_S = 280
COMMAND_TIMEOUT_S = 60.0
CONVERGE_S = 15.0
SETTLE_MS = 300

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt, QTimer
    from PySide6.QtGui import QAction, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import (QApplication, QComboBox, QDialogButtonBox, QLabel, QLineEdit, QMainWindow,
                                   QMessageBox, QSpinBox, QWidget)

    import guitest as gt
    from guitest import check, report, require
    import roto_gui as rg

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    SYNC = os.environ.get("M98_SYNC") or os.path.join(gt.OUT, "sync")
    if not os.path.isdir(SYNC):
        os.makedirs(SYNC)
    _keep = []

    # ---- plumbing --------------------------------------------------------------------------

    def main_gui():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def activate():
        win = main_gui()
        if win is None:
            return
        win.activateWindow()
        win.raise_()
        deadline = time.time() + 10.0
        while not win.isActiveWindow() and time.time() < deadline:
            QApplication.processEvents()
            time.sleep(0.02)

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def send_key(widget, key, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, key, modifiers))
        QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, key, modifiers))
        QApplication.processEvents()

    # An action is only used while its window's wrapper is held: PySide marks a child's wrapper
    # deleted once its parent's is collected, so no action wrapper leaves these helpers.
    def action_info(object_name):
        """The File action's enabled state and text, or None when there is no such action."""
        win = main_gui()
        a = win.findChild(QAction, object_name) if win is not None else None
        if a is None:
            return None
        return {"enabled": a.isEnabled(), "text": a.text()}

    def action_enabled(object_name):
        info = action_info(object_name)
        return info is not None and info["enabled"]

    def trigger_action(object_name):
        win = main_gui()
        a = win.findChild(QAction, object_name) if win is not None else None
        if a is None:
            return False
        a.trigger()
        return True

    def status_text():
        win = main_gui()
        label = win.findChild(QLabel, "liveSessionStatus") if win is not None else None
        if label is None or not label.isVisible():
            return ""
        return label.text()

    def write_file(path, text):
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            f.write(text)
        os.replace(tmp, path)

    def read_file(path):
        with open(path) as f:
            return f.read()

    def fill_dialog(object_name, fields, seen):
        """Fills the modal dialog with that object name once it is up and clicks its OK button."""
        def attempt(tries):
            dialog = QApplication.activeModalWidget()
            if dialog is None or dialog.objectName() != object_name:
                if tries > 0:
                    QTimer.singleShot(200, lambda: attempt(tries - 1))
                return
            _keep.append(dialog)
            seen["dialog"] = True
            for name, value in fields.items():
                w = dialog.findChild(QWidget, name)
                if w is None:
                    seen.setdefault("missing", []).append(name)
                    continue
                _keep.append(w)
                if isinstance(w, QComboBox):
                    if w.isEditable():
                        w.setEditText(value)
                    else:
                        index = w.findData(value)
                        if index < 0:
                            seen.setdefault("missing", []).append(name + "=" + value)
                        w.setCurrentIndex(index)
                elif isinstance(w, QSpinBox):
                    w.setValue(int(value))
                elif isinstance(w, QLineEdit):
                    w.setText(value)
            warning = dialog.findChild(QLabel, "securityWarning")
            seen["warning"] = warning.text() if warning is not None else None
            box = dialog.findChild(QDialogButtonBox)
            ok = box.button(QDialogButtonBox.Ok) if box is not None else None
            seen["ok_enabled"] = ok is not None and ok.isEnabled()
            if ok is not None and ok.isEnabled():
                ok.click()
            else:
                dialog.reject()

        QTimer.singleShot(200, lambda: attempt(50))

    def panel_of(name):
        node = app.getNode(name)
        if node is None:
            return None
        label = node.getLabel()
        for p in gt.widgets_of("NodeSettingsPanel"):
            try:
                if not p.isVisible():
                    continue
                for le in p.findChildren(QLineEdit):
                    if gt.class_name(le) == "LineEdit" and le.text() == label:
                        return p
            except RuntimeError:
                continue
        return None

    def open_panel(name):
        for attempt in range(2):
            gt.close_all_panels()
            yield ("sleep", SETTLE_MS)
            node = app.getNode(name)
            if node is None:
                return False
            app.clearSelection()
            activate()
            app.selectNode(node, True)
            QApplication.processEvents()
            view = graph_view()
            if view is not None:
                QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
            ok = yield ("until", lambda: panel_of(name) is not None, 10.0)
            if ok:
                return True
            report("NOTE the settings panel of %s did not open (attempt %d)" % (name, attempt + 1))
        return False

    def field_of(name, tooltip_part):
        """The first field of the parameter whose tooltip has tooltip_part in the node's panel."""
        p = panel_of(name)
        if p is None:
            return None
        for w in p.findChildren(QWidget):
            try:
                if w.objectName() == "dim-0" and w.isVisible() and tooltip_part in w.toolTip():
                    return w
            except RuntimeError:
                continue
        return None

    def wait_field(name, tooltip_part):
        """Waits for the field, which the panel builds over later event-loop passes than it shows in.
        Returns the field, or None with what the panel holds reported."""
        ok = yield ("until", lambda: field_of(name, tooltip_part) is not None, 10.0)
        if ok:
            return field_of(name, tooltip_part)
        p = panel_of(name)
        if p is not None:
            fields = [w for w in p.findChildren(QWidget) if w.objectName() == "dim-0"]
            matching = [w for w in fields if tooltip_part in w.toolTip()]
            report("NOTE the panel of %s has %d first fields, %d matching %r, %d of those visible"
                   % (name, len(fields), len(matching), tooltip_part, len([w for w in matching if w.isVisible()])))
        return None

    def knob_menu(field, entry):
        """Triggers the entry with that exact text in the field's right-click menu."""
        _keep.append(field)
        seen = {"triggered": False}

        def inspect():
            popup = QApplication.activePopupWidget()
            if popup is None:
                return
            _keep.append(popup)
            a = next((a for a in popup.actions() if a.text() == entry), None)
            if a is not None and a.isEnabled():
                a.trigger()
                seen["triggered"] = True
            popup.close()

        QTimer.singleShot(800, inspect)
        # The menu runs a nested loop until inspect() closes it.
        field.customContextMenuRequested.emit(field.rect().center())
        return seen["triggered"]

    def node_point(graph, name):
        node = app.getNode(name)
        x, y = node.getPosition()
        w, h = node.getSize()
        graph.resetTransform()
        graph.centerOn(x + w / 2.0, y + h / 2.0)
        QApplication.processEvents()
        return graph.mapFromScene(QPointF(x + w / 2.0, y + h / 2.0))

    def drag(widget, start, end, steps=6):
        def mouse(kind, p, button, buttons):
            QApplication.sendEvent(widget, QMouseEvent(kind, QPointF(p), button, buttons, Qt.NoModifier))
            QApplication.processEvents()

        mouse(QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
        mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, steps + 1):
            p = QPointF(start.x() + (end.x() - start.x()) * i / steps, start.y() + (end.y() - start.y()) * i / steps)
            mouse(QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton)
        mouse(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)

    def bundle_members(path):
        """The bundle's members by their path below its root folder, the manifest left out and the
        current frame zeroed."""
        members = {}
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                rel = info.filename.split("/", 1)[1] if "/" in info.filename else info.filename
                if rel in ("mimetype", "manifest.yaml"):
                    continue
                data = z.read(info)
                if rel == "project.yaml":
                    data = re.sub(rb"(timeline:\s*\{\s*current:\s*)-?\d+", rb"\g<1>0", data)
                members[rel] = data
        return members

    def dump(path):
        if os.path.exists(path):
            os.remove(path)
        return app.saveTempProject(path) and os.path.isfile(path)

    # ---- client ----------------------------------------------------------------------------

    def client_join(port):
        if not action_enabled("actionJoinLiveSession"):
            return "no enabled Join Live Session action"
        seen = {}
        fill_dialog("joinLiveSessionDialog", {"host": "127.0.0.1", "port": port, "displayName": "client"}, seen)
        trigger_action("actionJoinLiveSession")
        if not seen.get("dialog"):
            return "the join dialog did not show"
        if seen.get("missing"):
            return "the join dialog lacks " + ", ".join(seen["missing"])
        ok = yield ("until", lambda: status_text().startswith("Live: joined"), 30.0)
        if not ok:
            return "the status bar never said joined (%r)" % status_text()
        title = main_gui().windowFilePath()
        if not title.startswith("Live: "):
            return "the window title is %r, not the session's" % title
        if not action_enabled("actionLeaveLiveSession") or action_enabled("actionHostLiveSession"):
            return "the session actions are not enabled for a joined session"
        return "ok"

    def client_slider(name):
        ok = yield from open_panel(name)
        if not ok:
            return "no panel for " + name
        field = yield from wait_field(name, "Mix factor")
        if field is None:
            return "no mix field in the panel of " + name
        slider = None
        a = field.parentWidget()
        for _ in range(4):
            if a is None:
                break
            sliders = [w for w in a.findChildren(QWidget) if gt.class_name(w) == "ScaleSliderQWidget" and w.isVisible()]
            if sliders:
                slider = sliders[0]
                break
            a = a.parentWidget()
        if slider is None:
            return "no slider beside the mix field"
        _keep.append(slider)
        before = app.getNode(name).getParam("mix").get()
        y = slider.height() / 2.0
        drag(slider, QPointF(slider.width() * 0.8, y), QPointF(slider.width() * 0.4, y))
        yield ("sleep", SETTLE_MS)
        after = app.getNode(name).getParam("mix").get()
        if abs(after - before) < 1e-6:
            return "the drag left mix at %g" % after
        return "ok"

    def client_key(name, frame):
        # The timeline is only reachable from Python through a viewer; the shared project has one.
        viewer = app.getViewer("Viewer1")
        if viewer is None:
            return "no Python handle on Viewer1 to move the timeline"
        viewer.seek(frame)
        ok = yield from open_panel(name)
        if not ok:
            return "no panel for " + name
        field = yield from wait_field(name, "<b>size</b>")
        if field is None:
            return "no size field in the panel of " + name
        if not knob_menu(field, "Set Key (all dimensions)"):
            return "the size menu offers no Set Key (all dimensions)"
        param = app.getNode(name).getParam("size")
        if param.getNumKeys(0) < 1:
            return "no key was set"
        return "ok"

    def client_move(name, dx, dy):
        gt.close_all_panels()
        activate()
        graph = graph_view()
        if graph is None:
            return "no node graph"
        before = app.getNode(name).getPosition()
        start = node_point(graph, name)
        drag(graph.viewport(), QPointF(start), QPointF(start.x() + dx, start.y() + dy))
        yield ("sleep", SETTLE_MS)
        after = app.getNode(name).getPosition()
        if abs(after[0] - before[0]) < 1.0 and abs(after[1] - before[1]) < 1.0:
            return "the drag did not move %s (%r)" % (name, after)
        return "ok"

    def client_create(source):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode("Grade", -1, None, props)
        if node is None:
            return "could not create a Grade"
        node.setScriptName("ClientGrade")
        node.setPosition(300, 300)
        src = app.getNode(source)
        if src is None or not node.connectInput(0, src):
            return "could not connect ClientGrade to " + source
        yield ("sleep", SETTLE_MS)
        return "ok"

    def client_roto():
        gt.close_all_panels()
        rg.activate()
        rp, ctx = rg.open_roto(app, "RotoPaint")
        if rp is None or ctx is None:
            return "could not create a RotoPaint"
        rp.setScriptName("ClientPaint")
        yield ("sleep", 1000)
        gl = rg.viewer_gl()
        if gl is None:
            return "no viewer"
        _keep.append(gl)
        if not rg.pick_tool("PaintBrushToolButton", "PaintSolidTool"):
            return "could not pick the solid brush"
        yield ("sleep", SETTLE_MS)
        y = gl.height() * 0.5
        points = [QPointF(gl.width() * (0.3 + 0.04 * i), y + 10 * (i % 2)) for i in range(11)]
        rg.press(gl, points[0])
        for p in points[1:]:
            yield ("sleep", 10)
            rg.move(gl, p, Qt.LeftButton)
        rg.release(gl, points[-1])
        yield ("sleep", SETTLE_MS)
        items = app.getNode("ClientPaint").getRotoContext().getBaseLayer().getChildren()
        if len(items) != 1:
            return "the stroke made %d items" % len(items)
        return "ok"

    def client_user_knob(name):
        node = app.getNode(name)
        if node is None:
            return "no " + name
        p = node.createDoubleParam("sharedAmount", "Shared Amount")
        if p is None:
            return "could not add the parameter"
        node.refreshUserParamsGUI()
        p.set(0.75)
        yield ("sleep", SETTLE_MS)
        return "ok"

    def client_ended():
        ok = yield ("until", lambda: status_text() == "" and action_enabled("actionJoinLiveSession"), 30.0)
        if not ok:
            return "the client still shows a session (%r)" % status_text()
        boxes = [w for w in QApplication.topLevelWidgets() if isinstance(w, QMessageBox)
                 and w.objectName() == "liveSessionEndedMessage" and w.isVisible()]
        if not boxes:
            return "the client was not told the session ended"
        _keep.extend(boxes)
        text = boxes[0].text()
        for b in boxes:
            b.close()
        title = main_gui().windowFilePath()
        if title.startswith("Live: "):
            return "the title still names the session (%r)" % title
        node = app.createNode("Blur")
        if node is None:
            return "the detached copy is not editable"
        node.getParam("size").set(3.0, 3.0)
        report("NOTE the client was told: " + text)
        return "ok"

    def client_command(line):
        words = line.split()
        verb = words[0]
        if verb == "join":
            return (yield from client_join(words[1]))
        if verb == "slider":
            return (yield from client_slider(words[1]))
        if verb == "key":
            return (yield from client_key(words[1], int(words[2])))
        if verb == "move":
            return (yield from client_move(words[1], float(words[2]), float(words[3])))
        if verb == "create":
            return (yield from client_create(words[1]))
        if verb == "roto":
            return (yield from client_roto())
        if verb == "userknob":
            return (yield from client_user_knob(words[1]))
        if verb == "ended":
            return (yield from client_ended())
        if verb == "dump":
            return "ok" if dump(words[1]) else "could not write " + words[1]
        if verb == "has":
            return "yes" if app.getNode(words[1]) is not None else "no"
        if verb == "quit":
            return "bye"
        return "unknown command " + verb

    def client_steps():
        ok = yield ("until", lambda: main_gui() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the client's main window is up")
        write_file(os.path.join(SYNC, "ready"), "ready")
        n = 0
        while True:
            n += 1
            path = os.path.join(SYNC, "cmd-%03d" % n)
            ok = yield ("until", lambda: os.path.exists(path), COMMAND_TIMEOUT_S * 3)
            require(ok, "command %d arrives from the host" % n)
            line = read_file(path).strip()
            try:
                result = yield from client_command(line)
            except Exception:
                result = "error: " + traceback.format_exc().replace("\n", " | ")
            check(result in ("ok", "yes", "no", "bye"), "client: %s -> %s" % (line, result))
            write_file(os.path.join(SYNC, "ack-%03d" % n), result)
            if line == "quit":
                return

    # ---- host ------------------------------------------------------------------------------

    _client = {"process": None, "n": 0}

    def client_do(line, timeout=COMMAND_TIMEOUT_S):
        _client["n"] += 1
        n = _client["n"]
        ack = os.path.join(SYNC, "ack-%03d" % n)
        write_file(os.path.join(SYNC, "cmd-%03d" % n), line)
        proc = _client["process"]
        ok = yield ("until", lambda: os.path.exists(ack) or proc.poll() is not None, timeout)
        if os.path.exists(ack):
            return read_file(ack)
        return "client exited" if proc.poll() is not None else "timeout"

    def converged(label):
        """Both sides' bundles must match within CONVERGE_S."""
        deadline = time.time() + CONVERGE_S
        attempt = 0
        host_members = client_members = None
        while True:
            attempt += 1
            host_path = os.path.join(SYNC, "host-%s-%d.ntpz" % (label, attempt))
            client_path = os.path.join(SYNC, "client-%s-%d.ntpz" % (label, attempt))
            if not dump(host_path):
                return check(False, "%s: the host wrote its bundle" % label)
            answer = yield from client_do("dump " + client_path)
            if answer != "ok":
                return check(False, "%s: the client wrote its bundle (%s)" % (label, answer))
            host_members = bundle_members(host_path)
            client_members = bundle_members(client_path)
            if host_members == client_members:
                return check(True, "%s: host and client projects match" % label)
            if time.time() > deadline:
                break
            yield ("sleep", 500)
        diff_path = os.path.join(gt.OUT, "diff-%s.txt" % label)
        import difflib
        with open(diff_path, "w") as f:
            for rel in sorted(set(host_members) | set(client_members)):
                a = host_members.get(rel, b"").decode("utf-8", "replace").splitlines()
                b = client_members.get(rel, b"").decode("utf-8", "replace").splitlines()
                if a != b:
                    f.write("\n".join(difflib.unified_diff(a, b, "host/" + rel, "client/" + rel, lineterm="")) + "\n")
        return check(False, "%s: host and client projects match (diff in %s)" % (label, diff_path))

    def host_rename(name, new_label):
        ok = yield from open_panel(name)
        if not check(ok, "host: opened the panel of " + name):
            return
        p = panel_of(name)
        label = app.getNode(name).getLabel()
        edits = [le for le in p.findChildren(QLineEdit) if gt.class_name(le) == "LineEdit" and le.text() == label]
        if not check(len(edits) > 0, "host: the panel of %s has its name field" % name):
            return
        edit = edits[0]
        _keep.append(edit)
        edit.setFocus()
        edit.setText(new_label)
        send_key(edit, Qt.Key_Return)
        yield ("sleep", SETTLE_MS)
        renamed = app.getNode(new_label)
        check(renamed is not None or (app.getNode(name) is not None and app.getNode(name).getLabel() == new_label),
              "host: %s is renamed %s" % (name, new_label))
        gt.close_all_panels()

    def host_delete(name):
        gt.close_all_panels()
        activate()
        node = app.getNode(name)
        if not check(node is not None, "host: %s exists before the delete" % name):
            return
        app.clearSelection()
        app.selectNode(node, True)
        graph = graph_view()
        graph.setFocus()
        send_key(graph, Qt.Key_Backspace)
        ok = yield ("until", lambda: app.getNode(name) is None, 10.0)
        check(ok, "host: Backspace in the graph deletes " + name)

    def host_steps():
        ok = yield ("until", lambda: main_gui() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        binary = os.environ.get("NATRON_GUI_TEST_BINARY", "")
        require(os.access(binary, os.X_OK), "the harness names the Natron binary (NATRON_GUI_TEST_BINARY)", binary)
        script = os.environ.get("NATRON_GUI_TEST_SCRIPT", "")
        require(os.path.isfile(script), "the harness names this script (NATRON_GUI_TEST_SCRIPT)", script)

        gt.close_all_panels()
        constant = app.createNode("Constant")
        grade = app.createNode("Grade")
        blur = app.createNode("Blur")
        doomed = app.createNode("Blur")
        require(constant is not None and grade is not None and blur is not None and doomed is not None,
                "create the starting comp")
        constant.setScriptName("Src")
        grade.setScriptName("Tint")
        blur.setScriptName("Soft")
        doomed.setScriptName("Doomed")
        constant.setPosition(0, 0)
        grade.setPosition(0, 150)
        blur.setPosition(0, 300)
        doomed.setPosition(250, 150)
        grade.connectInput(0, constant)
        blur.connectInput(0, grade)
        doomed.connectInput(0, constant)
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer node exists")
        viewer.connectInput(0, blur)
        project_file = os.path.join(gt.OUT, "m98_host.ntpz")
        require(app.saveProjectAs(project_file), "save the host project to " + project_file)

        # ---- host
        gt.section("host")
        require(action_enabled("actionHostLiveSession"), "File has an enabled Host Live Session action")
        seen = {}
        # Port 0 is Any Free Port: the host listens on one the system chooses, so no other process can
        # take it between choosing and listening.
        fill_dialog("hostLiveSessionDialog", {"bindAddress": "127.0.0.1", "port": "0", "displayName": "host"}, seen)
        trigger_action("actionHostLiveSession")
        require(seen.get("dialog"), "Host Live Session shows its dialog")
        check(not seen.get("missing"), "the host dialog has Bind Address, Port and Display Name (%s)" % seen.get("missing"))
        warning = seen.get("warning") or ""
        check("no password" in warning and "trust" in warning, "the host dialog warns that anyone can join: %r" % warning)
        hosted = re.compile(r"Live: hosting on 127\.0\.0\.1:(\d+)")
        ok = yield ("until", lambda: hosted.match(status_text()) is not None, 10.0)
        require(ok, "the status bar says the project is hosted on a port", status_text())
        port = int(hosted.match(status_text()).group(1))
        check(port > 0, "the host listens on the port the system chose: %d" % port)
        check(action_enabled("actionLeaveLiveSession") and not action_enabled("actionJoinLiveSession"),
              "while hosting, only Leave is enabled")

        # ---- join
        gt.section("join")
        client_out = os.path.join(gt.OUT, "client")
        if not os.path.isdir(client_out):
            os.makedirs(client_out)
        for leftover in ("results.txt", "natron.log"):
            if os.path.exists(os.path.join(client_out, leftover)):
                os.remove(os.path.join(client_out, leftover))
        for leftover in os.listdir(SYNC):
            os.remove(os.path.join(SYNC, leftover))
        env = dict(os.environ)
        env["M98_ROLE"] = "client"
        env["M98_SYNC"] = SYNC
        env["NATRON_GUI_TEST_OUT"] = client_out
        log = open(os.path.join(client_out, "natron.log"), "w")
        _client["process"] = subprocess.Popen([binary, script], env=env, stdout=log,
                                              stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        proc = _client["process"]
        ok = yield ("until", lambda: os.path.exists(os.path.join(SYNC, "ready")) or proc.poll() is not None,
                    STARTUP_TIMEOUT_S)
        require(ok and proc.poll() is None, "the client Natron starts")
        answer = yield from client_do("join %d" % port)
        require(answer == "ok", "the client joins through Join Live Session", answer)
        ok = yield ("until", lambda: "1 user" in status_text(), 10.0)
        check(ok, "the host's status bar counts the client: %r" % status_text())
        yield from converged("joined")

        # ---- client edits
        gt.section("client-edits")
        for label, line in (("slider", "slider Tint"),
                            ("keyframe", "key Soft 12"),
                            ("move", "move Soft 120 60"),
                            ("create", "create Tint"),
                            ("roto", "roto"),
                            ("user-knob", "userknob Soft")):
            answer = yield from client_do(line)
            if check(answer == "ok", "client %s: %s" % (label, answer)):
                yield from converged(label)

        # ---- host edits
        gt.section("host-edits")
        yield from host_rename("Tint", "Tinted")
        yield from converged("rename")
        yield from host_delete("Doomed")
        yield from converged("delete")
        answer = yield from client_do("has Doomed")
        check(answer == "no", "the client lost the node the host deleted (%s)" % answer)

        # ---- stop
        gt.section("stop")
        leave = action_info("actionLeaveLiveSession")
        require(leave is not None and leave["enabled"], "Stop Hosting Session is enabled")
        check(leave["text"] == "Stop Hosting Session", "the host's leave action reads Stop Hosting Session (%r)" % leave["text"])
        trigger_action("actionLeaveLiveSession")
        ok = yield ("until", lambda: status_text() == "" and action_enabled("actionHostLiveSession"), 10.0)
        check(ok, "the host's status bar clears and Host is enabled again")
        check(main_gui().windowFilePath().endswith("m98_host.ntpz"), "the host keeps its own file")
        answer = yield from client_do("ended")
        check(answer == "ok", "the client keeps an unsaved, editable copy: %s" % answer)
        answer = yield from client_do("quit")
        check(answer == "bye", "the client quits (%s)" % answer)

    def host_cleanup():
        proc = _client["process"]
        if proc is None:
            return
        try:
            proc.wait(30)
        except Exception:
            proc.kill()
            proc.wait(10)
        results = os.path.join(gt.OUT, "client", "results.txt")
        lines = read_file(results).splitlines() if os.path.exists(results) else []
        for line in lines:
            if line.startswith("FAIL "):
                check(False, "client " + line[5:])
        summary = [line for line in lines if line.startswith("SUMMARY ")]
        check(bool(summary) and summary[-1] == "SUMMARY 0 failure(s)",
              "the client process passed its own checks (%s)" % (summary[-1] if summary else "no results.txt"))
        check(proc.returncode == 0, "the client process exits 0 (%s)" % proc.returncode)

    if ROLE == "client":
        gt.run(client_steps())
    else:
        _finish = gt.finish

        def finish_with_client():
            gt.finish = _finish
            try:
                host_cleanup()
            except Exception:
                check(False, "client cleanup error: " + traceback.format_exc())
            _finish()

        gt.finish = finish_with_client
        gt.run(host_steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
