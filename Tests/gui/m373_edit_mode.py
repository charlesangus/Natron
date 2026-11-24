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
import zipfile
import zlib

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

import guitest as gt
from guitest import check

import knob_edit_common as cm

from PySide6.QtCore import QMetaObject
from PySide6.QtWidgets import QWidget


def palette_visible(name):
    return cm.visible_child(name, QWidget, "KnobEditPalette") is not None


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
    gt.require(cm.toggle_from_menu(blur), "ran Edit parameters")
    ok = yield ("until", lambda: not cm.is_editing(blur), 5.0)
    check(ok, "Edit parameters turns edit mode off")
    check(not cm.pencil(blur).isChecked(), "the pencil follows the menu entry")
    check(not palette_visible(blur), "the palette is gone after leaving edit mode")
    check(cm.param(blur, "float1") is not None, "the user float survives leaving edit mode")
    cm.toggle_from_menu(blur)
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
    app.selectNode(node, True)
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
    cm.toggle_from_menu(blur)
    yield ("sleep", 300)
    check(not cm.is_editing(blur), "Edit parameters does nothing on that node")
    gt.shot("m373_edit_not_editable.png")


cm.start(steps())
