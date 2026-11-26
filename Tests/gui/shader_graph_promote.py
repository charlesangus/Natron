"""A GUI test of promoting and demoting a shader node's parameter from its settings panel, inside
a USD Material Builder. It is driven by Tests/gui/run-gui-test.sh (1920x1080), not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m351-gui/promote GUI_TEST_TIMEOUT=300 \
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_promote.py <AppImage or build/fast/App/Natron>'

The port toggle beside a BSDF's diffuse roughness (not a default port) promotes it; a math node is connected to the new port,
which greys the roughness fields out; the toggle then asks before demoting the connected port, the
question is answered Yes, and the port goes with its connection, leaving the fields editable.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 280
SETTLE_MS = 500
MAX_PANES = 40
POLL_TIMEOUT_S = 10.0
QUESTION_POLL_MS = 100
QUESTION_POLLS = 100
BUILDER = "USDMaterialBuilder"
MATH = "cycles_math"
BSDF = "cycles_principled_bsdf"
PORT = "diffuse_roughness"

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPointF, Qt, QTimer
    from PySide6.QtGui import QFocusEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QDialogButtonBox, QPushButton

    import guitest as gt
    from guitest import check

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def create(plugin_id, x, y, group=None):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, group, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def graph_of(group):
        found = [g for g in gt.widgets_of("NodeGraph") if g.objectName() == group.getLabel()]
        return found[-1] if found else None

    def show_graph_tab(label):
        for i in range(1, MAX_PANES + 1):
            pane = app.getTabWidget("pane%d" % i)
            if pane is None:
                continue
            for t in range(pane.count()):
                if pane.getTabLabel(t) == label:
                    pane.setCurrentIndex(t)
                    QApplication.processEvents()
                    return True
        return False

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if not w.objectName() and w.isVisible()]
        return graphs[0] if graphs else None

    def open_graph(group):
        """Shows group's graph tab, diving into the group from the root graph (Shift+double-click)
        when it has none."""
        if show_graph_tab(group.getLabel()):
            return True
        root = root_graph()
        if root is None:
            return False
        # The dive-in appends the tab to the pane of the last focused graph.
        QApplication.sendEvent(root, QFocusEvent(QEvent.FocusIn, Qt.OtherFocusReason))
        x, y = group.getPosition()[0:2]
        w, h = group.getSize()[0:2]
        centre = QPointF(x + w / 2.0, y + h / 2.0)
        root.centerOn(centre)
        QApplication.processEvents()
        pos = QPointF(root.mapFromScene(centre))
        event = QMouseEvent(QEvent.MouseButtonDblClick, pos, Qt.LeftButton, Qt.LeftButton, Qt.ShiftModifier)
        QApplication.sendEvent(root.viewport(), event)
        QApplication.processEvents()
        return show_graph_tab(group.getLabel())

    def input_index(node, port):
        for i in range(node.getMaxInputCount()):
            if node.getInputLabel(i) == port:
                return i
        return -1

    def port_button(builder, node, knob):
        name = "networkPort:%s.%s.%s" % (builder.getScriptName(), node.getScriptName(), knob)
        for w in QApplication.allWidgets():
            try:
                if isinstance(w, QPushButton) and w.objectName() == name and w.isVisible():
                    return w
            except RuntimeError:
                continue
        return None

    def fields_of(knob):
        """The visible spin boxes of knob, found by its script name in their tool tips."""
        tag = "<b>%s</b>" % knob
        found = []
        for w in QApplication.allWidgets():
            try:
                if gt.class_name(w) == "SpinBox" and w.isVisible() and tag in w.toolTip():
                    found.append(w)
            except RuntimeError:
                continue
        return found

    def fields_read_only(knob):
        fields = fields_of(knob)
        return [f.isReadOnly() for f in fields]

    def answer_question(seen):
        """Polls, from inside the question's own modal loop, for the question and answers Yes."""
        timer = QTimer()
        timer.setInterval(QUESTION_POLL_MS)
        polls = {"left": QUESTION_POLLS}

        def poll():
            try:
                polls["left"] -= 1
                dialogs = [w for w in QApplication.topLevelWidgets()
                           if w.isVisible() and gt.class_name(w) == "MessageBox"]
                if not dialogs:
                    if polls["left"] <= 0:
                        timer.stop()
                    return
                timer.stop()
                seen["question"] = True
                boxes = dialogs[0].findChildren(QDialogButtonBox)
                yes = boxes[0].button(QDialogButtonBox.Yes) if boxes else None
                if yes is not None:
                    seen["yes"] = True
                    yes.click()
                else:
                    dialogs[0].reject()
            except Exception:
                timer.stop()
                check(False, "answering the question: " + traceback.format_exc())

        timer.timeout.connect(poll)
        timer.start()
        return timer

    def main():
        gt.section("build")
        builder = create(BUILDER, 0, 0)
        yield ("sleep", SETTLE_MS)
        gt.require(open_graph(builder), "the builder's graph has a tab")
        ok = yield ("until", lambda: graph_of(builder) is not None and graph_of(builder).isVisible(), POLL_TIMEOUT_S)
        gt.require(ok, "the builder's graph is shown")
        math = create(MATH, 0, 0, builder)
        bsdf = create(BSDF, 400, 0, builder)
        yield ("sleep", SETTLE_MS)
        gt.close_all_panels()
        app.selectNode(bsdf, True)
        QMetaObject.invokeMethod(graph_of(builder), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: port_button(builder, bsdf, PORT) is not None, POLL_TIMEOUT_S)
        gt.require(ok, "the BSDF's panel shows a port toggle beside %s" % PORT)
        button = port_button(builder, bsdf, PORT)
        check(PORT not in list(bsdf.getPorts()), "%s starts as a parameter only" % PORT)
        check(not button.isChecked(), "the toggle starts unchecked")
        check(bool(fields_of(PORT)), "the panel shows %s's fields" % PORT)

        gt.section("promote")
        button.click()
        yield ("sleep", SETTLE_MS)
        check(PORT in list(bsdf.getPorts()), "the toggle promotes %s" % PORT)
        button = port_button(builder, bsdf, PORT)
        check(button is not None and button.isChecked(), "the toggle shows the port as promoted")

        gt.section("connect")
        index = input_index(bsdf, PORT)
        gt.require(index >= 0, "%s is an input of the BSDF" % PORT)
        check(bsdf.connectInput(index, math), "connect the math node to %s" % PORT)
        yield ("sleep", SETTLE_MS)
        check(bsdf.getInput(index) is not None, "%s is connected" % PORT)
        check(not bsdf.getParam(PORT).getIsEnabled(0), "the connected port's parameter is disabled")
        read_only = fields_read_only(PORT)
        check(bool(read_only) and all(read_only), "the panel greys %s's fields out (got %r)" % (PORT, read_only))
        gt.shot("shader_graph_promote_connected.png")

        gt.section("demote through the question")
        seen = {}
        timer = answer_question(seen)
        button = port_button(builder, bsdf, PORT)
        gt.require(button is not None, "the toggle is still there")
        button.click()
        timer.stop()
        yield ("sleep", SETTLE_MS)
        check(seen.get("question", False), "demoting a connected port asks first")
        check(seen.get("yes", False), "the question offers Yes")
        check(PORT not in list(bsdf.getPorts()), "the answered question demotes %s" % PORT)
        check(input_index(bsdf, PORT) < 0, "%s is no longer an input" % PORT)
        check(bsdf.getParam(PORT).getIsEnabled(0), "the demoted parameter is enabled again")
        read_only = fields_read_only(PORT)
        check(bool(read_only) and not any(read_only), "the panel's %s fields are editable again (got %r)" % (PORT, read_only))
        button = port_button(builder, bsdf, PORT)
        check(button is not None and not button.isChecked(), "the toggle shows the port as demoted")
        gt.shot("shader_graph_promote_demoted.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(main())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
