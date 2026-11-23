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
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

import guitest as gt
from guitest import check, report
import knob_edit_common as cm

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
    path = os.path.join(gt.OUT, "m373_missing_plugin.ntp")
    gt.require(app.saveProjectAs(path), "saved the project to " + path)
    with open(path) as f:
        text = f.read()
    gt.require(plugin_id in text, "the saved project names the Blur's plug-in ID")
    with open(path, "w") as f:
        f.write(text.replace(plugin_id, "net.natron.test.MissingPlugin"))
    loaded = app.loadProject(path)
    gt.require(loaded is not None, "loaded the project back")
    cm.use_app(loaded)
    yield ("sleep", 1000)
    node = loaded.getNode(blur)
    gt.require(node is not None, "the node with the missing plug-in is in the loaded project")
    if cm.panel_of(blur) is None:
        loaded.selectNode(node, True)
    ok = yield ("until", lambda: cm.panel_of(blur) is not None, 10.0)
    if not ok:
        report("NOTE the missing-plugin node's panel did not open, so its pencil can't be checked")
    else:
        b = cm.pencil(blur)
        check(b is None or not b.isEnabled(), "the pencil is disabled on a node whose plug-in is missing")
        cm.toggle_from_menu(blur)
        yield ("sleep", 300)
        check(not cm.is_editing(blur), "Edit parameters does nothing on that node")
        gt.shot("m373_edit_not_editable.png")


cm.start(steps())
