"""A GUI test of reordering in the knob editor: a knob dragged into a group, a group dragged to
another user page, a user page tab dragged before a built-in page, and a palette entry dropped at a
position; the order is checked after each move and after its undo. Driven by
Tests/gui/run-gui-test.sh.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m373-reorder/out GUI_TEST_TIMEOUT=240 \\
        Tests/gui/run-gui-test.sh Tests/gui/m373_reorder.py <AppImage or build/fast/App/Natron>'

Knob and palette drags are sent to the editor's overlay as drag/drop events carrying the editor's
own payload, which is what its drag starts; page tabs are dragged with mouse events on the tab bar.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

import guitest as gt
from guitest import check
import knob_edit_common as cm

from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
from PySide6.QtGui import QMouseEvent
from PySide6.QtWidgets import QApplication, QTabWidget


def parent_of(node, knob):
    p = cm.param(node, knob)
    parent = p.getParent() if p is not None else None
    return parent.getScriptName() if parent is not None else None


def drop_move(node, knob, target_label, fy):
    ov = cm.overlay(node)
    w = cm.label_widget(node, target_label)
    if ov is None or w is None:
        return False
    return cm.send_drop(ov, cm.point_in(w, ov, fy), cm.mime_of(cm.EDITOR_MIME, cm.editor_payload(node, "move", knob)))


def drop_at_end(node, kind, value):
    ov = cm.overlay(node)
    if ov is None:
        return False
    return cm.send_drop(ov, QPoint(ov.width() // 2, ov.height() - 4),
                        cm.mime_of(cm.EDITOR_MIME, cm.editor_payload(node, kind, value)))


def tab_bar(node):
    p = cm.panel_of(node)
    if p is None:
        return None
    for t in p.findChildren(QTabWidget):
        if t.isVisible() and t.count() > 1:
            return t.tabBar()
    return None


def drag_tab(node, label, to_index):
    bar = tab_bar(node)
    if bar is None:
        return False
    src = [i for i in range(bar.count()) if cm.plain_text(bar.tabText(i)) == label]
    if not src:
        return False
    start = QPointF(bar.tabRect(src[0]).center())
    end = QPointF(bar.tabRect(to_index).left() + 2, start.y())
    mid = QPointF((start.x() + end.x()) / 2.0, start.y())
    QApplication.sendEvent(bar, QMouseEvent(QEvent.MouseButtonPress, start, bar.mapToGlobal(start), Qt.LeftButton,
                                            Qt.LeftButton, Qt.NoModifier))
    for p in (mid, end):
        QApplication.sendEvent(bar, QMouseEvent(QEvent.MouseMove, p, bar.mapToGlobal(p), Qt.NoButton, Qt.LeftButton,
                                                Qt.NoModifier))
    QApplication.sendEvent(bar, QMouseEvent(QEvent.MouseButtonRelease, end, bar.mapToGlobal(end), Qt.LeftButton,
                                            Qt.NoButton, Qt.NoModifier))
    QApplication.processEvents()
    return True


def steps():
    yield from cm.wait_for_graph()
    blur = yield from cm.open_blur()
    for entry, name in (("Float", "float1"), ("Group", "group1"), ("Float", "float2")):
        cm.add(blur, entry)
        ok = yield ("until", lambda: cm.selected(blur) == name, 5.0)
        gt.require(ok, "added " + name)
    page = parent_of(blur, "float1")
    gt.require(page is not None and cm.child_names(blur, page) == ["float1", "group1", "float2"],
               "the user page holds float1, group1, float2", "%r: %r" % (page, cm.child_names(blur, page)))

    gt.section("knob into a group")
    yield ("sleep", 300)
    gt.require(drop_move(blur, "float2", "Group 1", 0.95), "dropped float2 on the lower half of group1")
    ok = yield ("until", lambda: cm.child_names(blur, "group1") == ["float2"], 5.0)
    check(ok, "float2 moved into group1 (%r)" % cm.child_names(blur, "group1"))
    gt.shot("m373_reorder_into_group.png")
    gt.require(cm.undo(), "undo")
    ok = yield ("until", lambda: cm.child_names(blur, page) == ["float1", "group1", "float2"], 5.0)
    check(ok, "one undo put float2 back after group1 (%r)" % cm.child_names(blur, page))

    gt.section("group to another page")
    cm.add(blur, "Page")
    ok = yield ("until", lambda: cm.selected(blur) == "page1", 5.0)
    gt.require(ok, "added page1")
    yield ("sleep", 300)
    gt.require(drop_at_end(blur, "move", "group1"), "dropped group1 on page1")
    ok = yield ("until", lambda: cm.child_names(blur, "page1") == ["group1"], 5.0)
    check(ok, "group1 moved to page1 (%r)" % cm.child_names(blur, "page1"))
    check("group1" not in cm.child_names(blur, page), "and left %s" % page)
    gt.require(cm.undo(), "undo")
    ok = yield ("until", lambda: "group1" in cm.child_names(blur, page), 5.0)
    check(ok, "one undo put group1 back on %s (%r)" % (page, cm.child_names(blur, page)))

    gt.section("user page before a built-in page")
    pages = cm.child_names(blur)
    first = pages[0]
    gt.require(first not in (page, "page1"), "the first page is a built-in one (%r)" % (pages,))
    gt.require(drag_tab(blur, "Page 1", 0), "dragged the Page 1 tab to the front")
    ok = yield ("until", lambda: cm.child_names(blur)[:1] == ["page1"], 5.0)
    check(ok, "page1 is now the first page (%r)" % cm.child_names(blur))
    check(cm.child_names(blur)[1:2] == [first], "the built-in %s follows it" % first)
    gt.shot("m373_reorder_page.png")
    gt.require(cm.undo(), "undo")
    ok = yield ("until", lambda: cm.child_names(blur) == pages, 5.0)
    check(ok, "one undo restored the page order (%r)" % cm.child_names(blur))

    gt.section("palette drop")
    gt.require(cm.select(blur, "float1"), "selected float1")
    yield ("sleep", 300)
    ov = cm.overlay(blur)
    w = cm.label_widget(blur, "Float 1")
    gt.require(ov is not None and w is not None, "found float1's row")
    gt.require(cm.send_drop(ov, cm.point_in(w, ov, 0.1), cm.mime_of(cm.EDITOR_MIME, cm.editor_payload(blur, "new", "Int"))),
               "dropped the Int entry on the upper half of float1")
    ok = yield ("until", lambda: cm.child_names(blur, page)[:2] == ["int1", "float1"], 5.0)
    check(ok, "int1 was created before float1 (%r)" % cm.child_names(blur, page))
    gt.require(cm.undo(), "undo")
    ok = yield ("until", lambda: cm.param(blur, "int1") is None, 5.0)
    check(ok, "one undo removed int1")

    gt.section("built-ins are not targets")
    gt.require(tab_bar(blur) is not None, "found the page tabs")
    tab_bar(blur).setCurrentIndex(0)
    yield ("sleep", 300)
    before = cm.child_names(blur, first)
    drop_at_end(blur, "move", "float1")
    yield ("sleep", 400)
    check(cm.child_names(blur, first) == before and parent_of(blur, "float1") == page,
          "dropping a user knob on a built-in page does nothing")


cm.start(steps())
