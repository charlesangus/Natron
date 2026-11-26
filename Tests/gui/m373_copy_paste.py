"""A GUI test of copying user knobs between panels in edit mode: a user page holding a group and
knobs is copied from one Blur with Ctrl+C and pasted into a second Blur's panel with Ctrl+V; the
definitions and values arrive, the clipboard carries both the Natron type and plain text, and one
Ctrl+Z removes the paste. Driven by Tests/gui/run-gui-test.sh.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m373-copy-paste/out GUI_TEST_TIMEOUT=240 \\
        Tests/gui/run-gui-test.sh Tests/gui/m373_copy_paste.py <AppImage or build/fast/App/Natron>'
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

import guitest as gt
from guitest import check
import knob_edit_common as cm

from PySide6.QtCore import QEvent, Qt
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import QApplication

USER_KNOBS_MIME = "application/x-natron-user-knobs"


def key(node, k, modifiers):
    ov = cm.overlay(node)
    if ov is None:
        return False
    ov.setFocus(Qt.OtherFocusReason)
    QApplication.sendEvent(ov, QKeyEvent(QEvent.KeyPress, k, modifiers))
    QApplication.sendEvent(ov, QKeyEvent(QEvent.KeyRelease, k, modifiers))
    QApplication.processEvents()
    return True


def steps():
    yield from cm.wait_for_graph()
    a = yield from cm.open_blur(0, 0)
    cm.add(a, "Page")
    ok = yield ("until", lambda: cm.selected(a) == "page1", 5.0)
    gt.require(ok, "added page1 to %s" % a)
    cm.add(a, "Group")
    ok = yield ("until", lambda: cm.selected(a) == "group1", 5.0)
    gt.require(ok, "added group1 to page1")
    cm.add(a, "Int")
    ok = yield ("until", lambda: cm.selected(a) == "int1", 5.0)
    gt.require(ok, "added int1 after group1")
    yield ("sleep", 300)
    ov = cm.overlay(a)
    w = cm.label_widget(a, "Group 1")
    gt.require(ov is not None and w is not None, "found group1's row")
    cm.send_drop(ov, cm.point_in(w, ov, 0.95), cm.mime_of(cm.EDITOR_MIME, cm.editor_payload(a, "new", "Float")))
    ok = yield ("until", lambda: cm.child_names(a, "group1") == ["float1"], 5.0)
    gt.require(ok, "dropped a Float into group1", "%r" % cm.child_names(a, "group1"))
    cm.param(a, "int1").set(4)
    cm.param(a, "float1").set(0.5)
    cm.param(a, "float1").setHelp("Half.")
    source_order = cm.child_names(a, "page1")
    yield ("sleep", 300)

    gt.section("copy")
    gt.require(cm.select(a, "page1"), "selected page1")
    gt.require(key(a, Qt.Key_C, Qt.ControlModifier), "pressed Ctrl+C in %s's panel" % a)
    mime = QApplication.clipboard().mimeData()
    ok = yield ("until", lambda: QApplication.clipboard().mimeData() is not None and
                QApplication.clipboard().mimeData().hasFormat(USER_KNOBS_MIME), 5.0)
    check(ok, "the clipboard holds user knobs")
    mime = QApplication.clipboard().mimeData()
    check(mime is not None and "group1" in mime.text(), "and their plain-text definition")

    gt.section("paste")
    b = yield from cm.open_blur(400, 0)
    gt.require(key(b, Qt.Key_V, Qt.ControlModifier), "pressed Ctrl+V in %s's panel" % b)
    ok = yield ("until", lambda: cm.param(b, "float1") is not None, 5.0)
    check(ok, "the paste made the knobs on %s" % b)
    gt.shot("m373_copy_paste.png")
    if ok:
        page = cm.param(b, "float1").getParent()
        page = page.getParent() if page is not None else None
        check(page is not None and cm.child_names(b, page.getScriptName()) == source_order,
              "the pasted page holds %r (%r)" % (source_order, cm.child_names(b, page.getScriptName()) if page else None))
        check(cm.child_names(b, "group1") == ["float1"], "the pasted group holds float1")
        check(cm.param(b, "int1").getValue(0) == 4, "int1 kept its value")
        check(abs(cm.param(b, "float1").getValue(0) - 0.5) < 1e-9, "float1 kept its value")
        check(cm.param(b, "float1").getHelp() == "Half.", "float1 kept its tooltip")
        check(cm.param(b, "float1").getTypeName() == "Double" and cm.param(b, "int1").getTypeName() == "Int",
              "the types came along")
    check(cm.param(a, "float1") is not None, "the source knobs are untouched")

    gt.section("undo")
    gt.require(cm.undo(), "undo")
    ok = yield ("until", lambda: cm.param(b, "float1") is None and cm.param(b, "group1") is None and
                cm.param(b, "int1") is None, 5.0)
    check(ok, "one Ctrl+Z removed the whole paste")

    gt.section("duplicate")
    gt.require(cm.select(a, "int1"), "selected int1")
    before = cm.child_names(a, "page1")
    gt.require(key(a, Qt.Key_D, Qt.ControlModifier), "pressed Ctrl+D")
    ok = yield ("until", lambda: len(cm.child_names(a, "page1")) == len(before) + 1, 5.0)
    check(ok, "Ctrl+D added one knob to page1")
    if ok:
        names = cm.child_names(a, "page1")
        dup = names[names.index("int1") + 1]
        check(dup not in before and cm.param(a, dup).getTypeName() == "Int",
              "an Int copy %s went right after int1 (%r)" % (dup, names))
        check(cm.param(a, dup).getValue(0) == 4, "with int1's value")

cm.start(steps())
