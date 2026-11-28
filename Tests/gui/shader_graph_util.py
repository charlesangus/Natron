"""Helpers the shader graph GUI scripts share: building a builder, showing its graph, and opening
the tab menu over a graph as a user's Tab would. Every helper takes the script's app, which only
the script's own globals hold.

Scene items are read through the graph's scene() and are never walked up with parentItem().
"""
import json
import os

import NatronEngine
from PySide6.QtCore import QEvent, QPointF, Qt, QTimer
from PySide6.QtGui import QCursor, QFocusEvent, QKeyEvent, QMouseEvent
from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QListView, QMainWindow, QMessageBox

import guitest as gt

MAX_PANES = 40
POLL_TIMEOUT_S = 20.0
BUILDER = "USDMaterialBuilder"
MANIFEST_DIR = os.path.join(gt.ROOT, "Engine", "Nodes", "Scene", "Shaders", "manifest")
RENDERERS = ("cycles", "moonray")


def manifest_nodes(renderer):
    with open(os.path.join(MANIFEST_DIR, renderer + ".json")) as f:
        return json.load(f)["nodes"]


def shader_targets():
    """{plugin ID: targets} of every shader node the manifests list."""
    found = {}
    for renderer in RENDERERS:
        for node in manifest_nodes(renderer):
            found[node["id"]] = tuple(node.get("targets", ()))
    return found


def create(app, plugin_id, x, y, group=None):
    props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
    node = app.createNode(plugin_id, -1, group, props)
    gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
    node.setPosition(x, y)
    return node


def graph_of(group):
    found = [g for g in gt.widgets_of("NodeGraph") if g.objectName() == group.getLabel()]
    return found[-1] if found else None


def show_graph_tab(app, label):
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


def show_view(app, view):
    """Makes the pane tab holding view current, as clicking its tab would."""
    if view.isVisible():
        return True
    pane = view.parentWidget()
    while pane is not None and gt.class_name(pane) != "TabWidget":
        pane = pane.parentWidget()
    tabs = app.getTabWidget(pane.objectName()) if pane is not None else None
    if tabs is None:
        return False
    for i in range(tabs.count()):
        tabs.setCurrentIndex(i)
        QApplication.processEvents()
        if view.isVisible():
            return True
    return False


def show_root_graph(app):
    """Shows the main graph and returns it, None when it cannot be shown."""
    for view in gt.widgets_of("NodeGraph"):
        if not view.objectName() and show_view(app, view):
            return view
    return None


def open_graph(app, group):
    """Shows group's graph tab, diving into the group from the root graph (Shift+double-click)
    when it has none."""
    names = [group.getLabel(), group.getScriptName()]
    names += [n.replace(" ", "_") for n in names]
    for name in names:
        if show_graph_tab(app, name):
            return True
    for view in gt.widgets_of("NodeGraph"):
        if view.objectName() in names and show_view(app, view):
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
    return any(show_graph_tab(app, name) for name in names)


def park_viewer(app):
    """Moves Viewer1 far from the origin, where the builder under test is placed, so a click on
    the builder cannot land on the viewer."""
    viewer = app.getNode("Viewer1")
    if viewer is not None:
        viewer.setPosition(4000, 4000)


def main_window():
    for w in QApplication.topLevelWidgets():
        if isinstance(w, QMainWindow) and w.isVisible():
            return w
    return None


def focus_graph(view, cursor_scene_pos=None):
    """Leaves the graph where a real user's key reaches it: its window active, its viewport
    focused and under the cursor, at cursor_scene_pos when given. A generator for gt.run()."""
    win = main_window()
    if win is not None:
        win.activateWindow()
        win.raise_()
        win = None
        ok = yield ("until", lambda: main_window() is not None and main_window().isActiveWindow(), 10.0)
        if not ok:
            return False
    view.setFocus()
    viewport = view.viewport()
    if cursor_scene_pos is not None:
        QCursor.setPos(viewport.mapToGlobal(view.mapFromScene(cursor_scene_pos)))
    elif not viewport.rect().contains(viewport.mapFromGlobal(QCursor.pos())):
        QCursor.setPos(viewport.mapToGlobal(viewport.rect().center()))
    QApplication.processEvents()
    return True


def visible_popup():
    shown = [d for d in gt.widgets_of("TabTabTabWidget") if d.isVisible()]
    return shown[0] if shown else None


def wait_for_popup():
    """The tab menu's popup, its line edit and its list once shown; Nones on a timeout."""
    ok = yield ("until", lambda: visible_popup() is not None, POLL_TIMEOUT_S)
    if not ok:
        return None, None, None
    popup = visible_popup()
    return popup, popup.findChildren(QLineEdit)[0], popup.findChildren(QListView)[0]


def key_click(widget, key, modifiers=Qt.NoModifier):
    """Sends a key press and release to widget (PySide6.QtTest is not bundled in the AppImage)."""
    for kind in (QEvent.KeyPress, QEvent.KeyRelease):
        QApplication.sendEvent(widget, QKeyEvent(kind, key, modifiers))


def open_tab_menu(view):
    """Presses Tab over view and returns what wait_for_popup() does."""
    focused = yield from focus_graph(view)
    if not focused:
        return None, None, None
    key_click(view, Qt.Key_Tab)
    found = yield from wait_for_popup()
    return found


def close_tab_menu(popup):
    key_click(popup.findChildren(QLineEdit)[0], Qt.Key_Escape)
    QApplication.processEvents()


def listed_plugin_ids(adapter):
    """The plug-in ID of every item the open tab menu lists, "" for a command."""
    ids = []
    for item in adapter._plugin.get_items():
        value = item["menuobj"].property("natronPluginID")
        ids.append(value if isinstance(value, str) else "")
    return ids


def target_label(popup):
    labels = [w for w in popup.findChildren(QLabel) if w.objectName() == "tabMenuNetworkTarget"]
    return labels[0].text() if labels and labels[0].isVisible() else ""


def input_index(node, port):
    for i in range(node.getMaxInputCount()):
        if node.getInputLabel(i) == port:
            return i
    return -1


def connect(source, output, dest, port):
    """Connects output of source to the port of dest, showing the port first if needed."""
    if port not in list(dest.getPorts()):
        gt.require(dest.showPort(port), "show %s.%s" % (dest.getScriptName(), port))
    index = input_index(dest, port)
    gt.require(index >= 0, "%s has an input for %s" % (dest.getScriptName(), port))
    gt.require(dest.connectInput(index, source), "connect %s to %s.%s"
               % (source.getScriptName(), dest.getScriptName(), port))
    gt.require(dest.setOutputForInput(port, output), "%s.%s reads %s.%s"
               % (dest.getScriptName(), port, source.getScriptName(), output))


def input_source(dest, port):
    """The script name of the node the port of dest is connected to, None when it is not."""
    index = input_index(dest, port)
    src = dest.getInput(index) if index >= 0 else None
    return src.getScriptName() if src is not None else None


def port_items(graph, node_name):
    """{"input": [names], "output": [names]} of the visible port items of node_name, a fully
    qualified name; port items carry their side, port name and node in data() keys 0, 1 and 2."""
    found = {"input": [], "output": []}
    for item in graph.scene().items():
        side = item.data(0)
        if side not in found or item.data(2) != node_name or not item.isVisible():
            continue
        found[side].append(item.data(1))
    return found


def answering(call):
    """Runs call() while a timer answers each modal message box: No to using an auto-save,
    Discard to saving edits, OK to a warning. Returns call()'s result and the texts answered."""
    asked = []

    def answer():
        box = QApplication.activeModalWidget()
        if not isinstance(box, QMessageBox):
            return
        if "auto-save" in box.text():
            choices = (QMessageBox.StandardButton.No,)
        else:
            choices = (QMessageBox.StandardButton.Discard, QMessageBox.StandardButton.Ok)
        for choice in choices:
            button = box.button(choice)
            if button is not None:
                asked.append(box.text())
                button.click()
                return

    timer = QTimer()
    timer.timeout.connect(answer)
    timer.start(200)
    try:
        result = call()
    finally:
        timer.stop()
        timer = None
    return result, asked
