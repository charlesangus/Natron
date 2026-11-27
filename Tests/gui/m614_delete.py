"""A GUI test of deleting in the knob editor: the property section's Delete button deletes the
selected knob, and a knob dragged onto the palette is deleted, the palette showing it while the
drag hovers; one undo brings each back at its position. Also saves screenshots of the selection
frame on the first knob of a page and on one mid-page. Driven by Tests/gui/run-gui-test.sh.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m614-delete/out GUI_TEST_TIMEOUT=240 \\
        Tests/gui/run-gui-test.sh Tests/gui/m614_delete.py <AppImage or build/fast/App/Natron>'

The drag onto the palette is sent to the palette as drag/drop events carrying the editor's own
payload, which is what a knob drag from the panel carries.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

import guitest as gt
from guitest import check
import knob_edit_common as cm

from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QDragEnterEvent, QDragLeaveEvent, QDragMoveEvent, QDropEvent
from PySide6.QtWidgets import QApplication, QPushButton, QWidget

KNOBS = ["float1", "float2", "float3"]


def palette(name):
    return cm.visible_child(name, QWidget, "KnobEditPalette")


def delete_button(name):
    return cm.visible_child(name, QPushButton, "KnobEditPropertiesDelete")


def drop_to_delete_shown(name):
    w = palette(name)
    return w is not None and bool(w.property("dropToDelete"))


def panel_shot(name, file_name):
    p = cm.panel_of(name)
    path = os.path.join(gt.OUT, file_name)
    if p is not None and p.grab().save(path):
        gt.report("SHOT " + path)
    else:
        gt.report("NOTE could not save screenshot " + path)


def hover_palette(name, mime):
    """Sends a drag enter and a move over the middle of the palette; returns whether it accepted."""
    w = palette(name)
    if w is None:
        return False
    p = QPoint(w.width() // 2, w.height() // 2)
    actions = Qt.CopyAction | Qt.MoveAction
    enter = QDragEnterEvent(p, actions, mime, Qt.LeftButton, Qt.NoModifier)
    QApplication.sendEvent(w, enter)
    move = QDragMoveEvent(p, actions, mime, Qt.LeftButton, Qt.NoModifier)
    QApplication.sendEvent(w, move)
    QApplication.processEvents()
    return move.isAccepted()


def drop_on_palette(name, mime):
    w = palette(name)
    if w is None:
        return False
    drop = QDropEvent(QPointF(w.width() / 2.0, w.height() / 2.0), Qt.CopyAction | Qt.MoveAction, mime, Qt.LeftButton,
                      Qt.NoModifier)
    QApplication.sendEvent(w, drop)
    accepted = drop.isAccepted()
    QApplication.processEvents()
    return accepted


def leave_palette(name):
    w = palette(name)
    if w is not None:
        QApplication.sendEvent(w, QDragLeaveEvent())
        QApplication.processEvents()


def steps():
    yield from cm.wait_for_graph()
    blur = yield from cm.open_blur()
    for name in KNOBS:
        cm.add(blur, "Float")
        ok = yield ("until", lambda: cm.selected(blur) == name, 5.0)
        gt.require(ok, "added " + name)
    page = cm.param(blur, "float1").getParent().getScriptName()
    gt.require(cm.child_names(blur, page) == KNOBS, "the user page holds %r" % KNOBS,
               "%r: %r" % (page, cm.child_names(blur, page)))

    gt.section("selection frame")
    gt.require(cm.select(blur, "float1"), "selected float1, the first knob of the page")
    yield ("sleep", 300)
    gt.shot("m614_selected_first.png")
    panel_shot(blur, "m614_selected_first_panel.png")
    gt.require(cm.select(blur, "float2"), "selected float2, mid-page")
    yield ("sleep", 300)
    gt.shot("m614_selected_mid.png")
    panel_shot(blur, "m614_selected_mid_panel.png")

    gt.section("delete button")
    gt.require(cm.select(blur, ""), "cleared the selection")
    yield ("sleep", 200)
    b = delete_button(blur)
    gt.require(b is not None, "the property section has a visible Delete button")
    check(not b.isEnabled(), "Delete is disabled with nothing selected")
    gt.require(cm.select(blur, "float2"), "selected float2")
    ok = yield ("until", lambda: delete_button(blur) is not None and delete_button(blur).isEnabled(), 3.0)
    gt.require(ok, "Delete is enabled with float2 selected")
    delete_button(blur).click()
    QApplication.processEvents()
    ok = yield ("until", lambda: cm.param(blur, "float2") is None, 5.0)
    check(ok, "the Delete button deleted float2")
    check(cm.child_names(blur, page) == ["float1", "float3"], "float1 and float3 are left (%r)" % cm.child_names(blur, page))
    gt.require(cm.undo(), "undo")
    ok = yield ("until", lambda: cm.child_names(blur, page) == KNOBS, 5.0)
    check(ok, "one undo put float2 back between float1 and float3 (%r)" % cm.child_names(blur, page))

    gt.section("drag onto the palette")
    yield ("sleep", 300)
    gt.require(not drop_to_delete_shown(blur), "the palette shows no delete cue before the drag")
    new_mime = cm.mime_of(cm.EDITOR_MIME, cm.editor_payload(blur, "new", "Int"))
    check(not hover_palette(blur, new_mime) and not drop_to_delete_shown(blur),
          "the palette refuses one of its own entries")
    check(not drop_on_palette(blur, new_mime), "and its drop")
    move_mime = cm.mime_of(cm.EDITOR_MIME, cm.editor_payload(blur, "move", "float1"))
    check(hover_palette(blur, move_mime), "the palette accepts a drag of float1")
    check(drop_to_delete_shown(blur), "and shows that dropping it deletes it")
    yield ("sleep", 200)
    gt.shot("m614_drop_to_delete.png")
    panel_shot(blur, "m614_drop_to_delete_panel.png")
    leave_palette(blur)
    check(not drop_to_delete_shown(blur), "the cue goes when the drag leaves the palette")
    check(cm.param(blur, "float1") is not None, "leaving deletes nothing")
    gt.require(hover_palette(blur, move_mime), "dragged float1 back over the palette")
    gt.require(drop_on_palette(blur, move_mime), "the palette took the drop of float1")
    ok = yield ("until", lambda: cm.param(blur, "float1") is None, 5.0)
    check(ok, "dropping float1 on the palette deleted it")
    check(not drop_to_delete_shown(blur), "the cue is gone after the drop")
    check(cm.child_names(blur, page) == ["float2", "float3"], "float2 and float3 are left (%r)" % cm.child_names(blur, page))
    gt.require(cm.undo(), "undo")
    ok = yield ("until", lambda: cm.child_names(blur, page) == KNOBS, 5.0)
    check(ok, "one undo put float1 back first on the page (%r)" % cm.child_names(blur, page))

    gt.section("delete a page")
    cm.add(blur, "Page")
    ok = yield ("until", lambda: cm.selected(blur) == "page1", 5.0)
    gt.require(ok, "added page1")
    pages = cm.child_names(blur)
    ok = yield ("until", lambda: delete_button(blur) is not None and delete_button(blur).isEnabled(), 3.0)
    gt.require(ok, "Delete is enabled with page1 selected")
    delete_button(blur).click()
    QApplication.processEvents()
    ok = yield ("until", lambda: cm.param(blur, "page1") is None, 5.0)
    check(ok, "the Delete button deleted page1")
    gt.require(cm.undo(), "undo")
    ok = yield ("until", lambda: cm.child_names(blur) == pages, 5.0)
    check(ok, "one undo put page1 back (%r)" % cm.child_names(blur))


cm.start(steps())
