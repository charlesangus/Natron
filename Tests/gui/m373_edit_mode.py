"""A GUI test of the panel's knob edit mode: the header pencil and the right-click "Edit parameters"
entry toggle it, user knobs are selectable in it and built-in knobs are not, and a node whose
parameters can't be edited has both entries disabled. Driven by Tests/gui/run-gui-test.sh.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m373-edit-mode/out GUI_TEST_TIMEOUT=240 \\
        Tests/gui/run-gui-test.sh Tests/gui/m373_edit_mode.py <AppImage or build/fast/App/Natron>'

The non-editable holder is a node whose plug-in is missing: the script saves a project with a
Blur, renames the Blur's plug-in ID in the file and loads it back.
"""
import os
import re
import sys
import traceback
import zipfile
import zlib

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

import guitest as gt
from guitest import check

import knob_edit_common as cm

from PySide6.QtCore import QMetaObject, QPoint, Qt, QTimer
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QWidget

EDIT_ITEM = "Edit parameters"

# Wrappers of the clicked widgets and of the popups are kept for the run: PySide marks a widget's
# child wrappers deleted once a temporary wrapper of the widget is collected.
_held = []


def palette_visible(name):
    return cm.visible_child(name, QWidget, "KnobEditPalette") is not None


def bare_point(widget):
    """A point of widget that no child widget covers; the panel only opens its menu when the widget
    under the cursor is the right-clicked row or page itself."""
    for fy in (0.5, 0.25, 0.75, 0.1, 0.9):
        for x in range(widget.width() - 3, 2, -4):
            pt = QPoint(x, int(widget.height() * fy))
            if QApplication.widgetAt(widget.mapToGlobal(pt)) is widget:
                return pt
    return None


def menu_target(name):
    """The widget and point a user right-clicks for the panel's menu: the editor overlay in edit
    mode, otherwise a bare spot of a row or page of the panel."""
    ov = cm.overlay(name)
    if ov is not None:
        return ov, QPoint(ov.width() // 2, max(1, ov.height() - 4))
    p = cm.panel_of(name)
    if p is None:
        return None, None
    for w in p.findChildren(QWidget):
        try:
            if gt.class_name(w) != "RightClickableWidget" or not w.isVisible():
                continue
        except RuntimeError:
            continue
        pt = bare_point(w)
        if pt is not None:
            return w, pt
    return None, None


def right_click_menu(name, choose=None):
    """Right-clicks the panel with a real mouse press, records the text, enabled and checked state
    of every entry of the menu that opens, then chooses the entry labelled choose or closes the
    menu. The panel's own menu runs inside the press, so the press is sent from a timer."""
    seen = {"entries": None, "chosen": False, "failed": False}
    widget, pt = menu_target(name)
    if widget is None:
        check(False, "found a spot of %s's panel to right-click" % name)
        seen["failed"] = True
        return seen
    _held.append(widget)

    def inspect():
        popup = QApplication.activePopupWidget()
        if popup is None:
            seen["failed"] = True
            return
        _held.append(popup)
        entries = dict((a.text(), (a.isEnabled(), a.isChecked())) for a in popup.actions() if a.text())
        action = next((a for a in popup.actions() if choose is not None and a.text() == choose), None)
        if action is None or not action.isEnabled():
            seen["entries"] = entries
            popup.close()
            return
        seen["chosen"] = True
        seen["entries"] = entries
        popup.setActiveAction(action)
        QTest.keyClick(popup, Qt.Key_Return)

    def press():
        try:
            QTest.mouseClick(widget, Qt.RightButton, Qt.NoModifier, pt)
        except RuntimeError:
            check(False, "the right click completed: " + traceback.format_exc())

    QTimer.singleShot(800, inspect)
    QTimer.singleShot(0, press)
    return seen


def open_menu(name, choose=None):
    seen = right_click_menu(name, choose)
    ok = yield ("until", lambda: seen["entries"] is not None or seen["failed"], 5.0)
    gt.require(ok and not seen["failed"], "%s's right-click menu opened" % name)
    return seen


def entry_state(seen, text):
    """(enabled, checked) of the menu entry labelled text, or None when the menu lacks it."""
    return seen["entries"].get(text)


def steps():
    yield from cm.wait_for_graph()

    gt.section("pencil")
    blur = yield from cm.open_blur(editing=False)
    check(cm.pencil(blur) is not None and cm.pencil(blur).isEnabled(), "the pencil is enabled on a Blur")
    check(not palette_visible(blur), "no palette outside edit mode")
    gt.shot("m373_edit_off.png")
    gt.require(cm.set_editing(blur, True), "clicked the pencil")
    ok = yield ("until", lambda: cm.is_editing(blur) and palette_visible(blur), 5.0)
    check(ok, "the pencil turns edit mode on and shows the palette")
    check(cm.properties(blur) is not None, "the property section is shown")
    check(cm.pencil(blur).isChecked(), "the pencil is checked in edit mode")
    gt.shot("m373_edit_on.png")
    seen = yield from open_menu(blur)
    check(entry_state(seen, EDIT_ITEM) == (True, True),
          "a built-in page's menu has %r enabled and checked (entries %r)" % (EDIT_ITEM, seen["entries"]))
    check(entry_state(seen, "Copy") == (False, False), "Copy is disabled on a built-in page (entries %r)" % (seen["entries"],))
    check(entry_state(seen, "Delete") == (False, False), "Delete is disabled on a built-in page")
    check(entry_state(seen, "Paste") is not None, "a built-in page's menu has Paste")
    yield ("sleep", 300)

    gt.section("selection")
    cm.add(blur, "Float")
    ok = yield ("until", lambda: cm.selected(blur) == "float1", 5.0)
    check(ok, "a new user float is selected (selected %r)" % cm.selected(blur))
    check(not cm.select(blur, "size"), "the built-in size can't be selected")
    yield ("sleep", 200)
    check(cm.selected(blur) == "float1", "selecting a built-in leaves the selection alone")
    check(cm.select(blur, "float1"), "the user float can be selected")
    pages = cm.child_names(blur)
    user_pages = [p for p in pages if cm.param(blur, p) is not None and "float1" in cm.child_names(blur, p)]
    check(len(user_pages) == 1, "the float lives on a user page (pages %r)" % (pages,))
    if user_pages:
        check(cm.select(blur, user_pages[0]), "the user page can be selected")
    gt.shot("m373_edit_selected.png")

    gt.section("menu")
    seen = yield from open_menu(blur, EDIT_ITEM)
    check(entry_state(seen, EDIT_ITEM) == (True, True),
          "the editor's menu has %r enabled and checked (entries %r)" % (EDIT_ITEM, seen["entries"]))
    gt.require(seen["chosen"], "chose %r from the editor's menu" % EDIT_ITEM)
    ok = yield ("until", lambda: not cm.is_editing(blur), 5.0)
    check(ok, "Edit parameters turns edit mode off")
    check(not cm.pencil(blur).isChecked(), "the pencil follows the menu entry")
    check(not palette_visible(blur), "the palette is gone after leaving edit mode")
    check(cm.param(blur, "float1") is not None, "the user float survives leaving edit mode")
    yield ("sleep", 300)
    seen = yield from open_menu(blur, EDIT_ITEM)
    check(entry_state(seen, EDIT_ITEM) == (True, False),
          "the panel's menu has %r enabled and unchecked outside edit mode (entries %r)" % (EDIT_ITEM, seen["entries"]))
    gt.require(seen["chosen"], "chose %r from the panel's menu" % EDIT_ITEM)
    ok = yield ("until", lambda: cm.is_editing(blur), 5.0)
    check(ok, "Edit parameters turns edit mode back on")
    cm.set_editing(blur, False)
    ok = yield ("until", lambda: not cm.is_editing(blur), 5.0)
    check(ok, "the pencil turns edit mode off")

    gt.section("close and delete")
    cm.set_editing(blur, True)
    yield ("until", lambda: cm.is_editing(blur), 5.0)
    other = yield from cm.open_blur(400, 0)
    cm.natron_app().getNode(other).destroy()
    yield ("sleep", 500)
    check(cm.panel_of(other) is None, "deleting a node in edit mode closes its panel")
    check(cm.is_editing(blur), "the other panel stays in edit mode")
    gt.close_all_panels()
    yield ("sleep", 400)
    check(not gt.any_panel_open(), "closing a panel in edit mode works")

    gt.section("not editable")
    app = cm.natron_app()
    plugin_id = app.getNode(blur).getPluginID()
    path = os.path.join(gt.OUT, "m373_missing_plugin.ntpz")
    gt.require(app.saveProjectAs(path), "saved the project to " + path)
    # Loading the file a window already has open only raises that window, so the project is closed first.
    gt.require(app.resetProject(), "closed the saved project")
    ok = yield ("until", lambda: not app.getChildren(), 10.0)
    gt.require(ok, "the closed project's nodes are gone")
    field = ('plugin: "%s"' % plugin_id).encode()
    with zipfile.ZipFile(path) as z:
        entries = [(i.filename, z.read(i.filename)) for i in z.infolist()]
    named = [n for n, data in entries if field in data]
    gt.require(bool(named), "the saved project names the Blur's plug-in ID")
    edited = dict((n, data.replace(field, b'plugin: "net.natron.test.MissingPlugin"')) for n, data in entries if n in named)
    # A manifest that no longer matches its members is a load warning, which would stop the load
    # on its summary dialog, so the edited members' sizes and CRCs are written into it.
    for n, data in entries:
        if n.endswith("manifest.yaml"):
            text = data.decode()
            for member, new in edited.items():
                text, count = re.subn(r'\{name: "%s", size: \d+, crc32: "[0-9a-f]{8}"\}' % re.escape(member.split("/")[-1]),
                                      '{name: "%s", size: %d, crc32: "%08x"}' % (member.split("/")[-1], len(new), zlib.crc32(new) & 0xffffffff),
                                      text)
                gt.require(count == 1, "the manifest lists %s (%r)" % (member, text))
            edited[n] = text.encode()
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        for n, data in entries:
            z.writestr(n, edited.get(n, data))
    # The empty project loads the file in place. A placeholder counts as a load error, so the
    # call returns None even though the nodes are restored.
    app.loadProject(path)
    yield ("sleep", 1000)
    node = app.getNode(blur)
    gt.require(node is not None, "the node with the missing plug-in is in the loaded project")
    gt.require(node.getPluginID() != plugin_id, "the loaded node is a placeholder, not a Blur (%s)" % node.getPluginID())
    import NatronGui
    NatronGui.natron.getGuiInstance(app.getAppID()).selectNode(node, True)
    graph = cm.root_graph()
    gt.require(graph is not None, "found the node graph")
    QMetaObject.invokeMethod(graph, "showSelectedNodeSettingsPanel")

    def panel_open():
        p = cm.panel_of(blur)
        return p is not None and p.isVisible()

    ok = yield ("until", panel_open, 10.0)
    gt.require(ok, "opened the missing-plugin node's panel")
    b = cm.pencil(blur)
    check(b is None or not b.isEnabled(), "the pencil is disabled on a node whose plug-in is missing")
    seen = yield from open_menu(blur)
    state = entry_state(seen, EDIT_ITEM)
    check(state is not None and not state[0],
          "the panel's menu has %r disabled on that node (entries %r)" % (EDIT_ITEM, seen["entries"]))
    yield ("sleep", 300)
    cm.toggle_from_menu(blur)
    yield ("sleep", 300)
    check(not cm.is_editing(blur), "Edit parameters does nothing on that node")
    gt.shot("m373_edit_not_editable.png")


cm.start(steps())
