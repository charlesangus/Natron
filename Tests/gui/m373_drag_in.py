"""A GUI test of bringing knobs into a panel in edit mode: dropping a Blur's size on a Group that has
no user page creates the User page with the knob on it in one undo entry, dragging it onto a user
page of the Group makes a linked user knob, a Ctrl-drop makes an independent copy, "Link knob..." links a knob of a
node inside the Group, and outside edit mode a Ctrl-drag between two panels still links the two
knobs as before. Driven by Tests/gui/run-gui-test.sh.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m373-drag-in/out GUI_TEST_TIMEOUT=240 \\
        Tests/gui/run-gui-test.sh Tests/gui/m373_drag_in.py <AppImage or build/fast/App/Natron>'

The drags are real: a press and a move on the source knob's label start the knob's own drag, and
a timer running inside the drag's event loop delivers the drop to the target and cancels the drag.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

import guitest as gt
from guitest import check
import knob_edit_common as cm

from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QDrag
from PySide6.QtWidgets import QAbstractButton, QApplication, QComboBox, QWidget

SIZE_LABEL = "Size"
USER_PAGE = "userNatron"


def set_value(p, v):
    if p.getNumDimensions() == 2:
        p.set(v, v)
    else:
        p.set(v)


def value(p):
    return p.getValue(0)


def drag_label_onto(source, target_widget_of, modifiers, drop_modifiers, result):
    """Drags source's Size label and drops it where target_widget_of() says; result gets whether
    the drop was accepted."""
    def during():
        target = target_widget_of()
        if target is None:
            result.append(False)
        else:
            widget, pos = target
            result.append(cm.send_drop(widget, pos, cm.mime_of(cm.KNOB_LINK_MIME, b""), drop_modifiers))
        QDrag.cancel()
    return cm.start_label_drag(source, SIZE_LABEL, modifiers, during)


def overlay_end(node):
    def target():
        ov = cm.overlay(node)
        return (ov, QPoint(ov.width() // 2, ov.height() - 4)) if ov is not None else None
    return target


def user_params(node):
    n = cm.natron_app().getNode(node)
    return [p.getScriptName() for p in n.getParams() if p.getIsPersistent()] if n is not None else []


def steps():
    yield from cm.wait_for_graph()
    blur = yield from cm.open_blur(0, 0, editing=False)
    group = yield from cm.open_blur(400, 0, plugin_id="Group")
    src = cm.param(blur, "size")
    gt.require(src is not None, "%s has a size" % blur)

    gt.section("a drop on a panel with no user page creates one")
    gt.require(cm.param(group, USER_PAGE) is None and cm.param(group, "page1") is None, "%s starts with no user page" % group)
    result = []
    gt.require(drag_label_onto(blur, overlay_end(group), Qt.NoModifier, Qt.NoModifier, result), "dragged %s's Size label" % blur)
    ok = yield ("until", lambda: cm.param(group, "size") is not None, 5.0)
    check(result[:1] == [True], "the built-in page accepted the drop")
    check(ok, "the drop made a size knob on %s" % group)
    check(cm.param(group, USER_PAGE) is not None, "the drop created the %s page" % USER_PAGE)
    check(cm.child_names(group, USER_PAGE) == ["size"], "size is on %s (%r)" % (USER_PAGE, cm.child_names(group, USER_PAGE)))
    gt.require(cm.undo(), "undo")
    ok = yield ("until", lambda: cm.param(group, "size") is None, 5.0)
    check(ok, "one undo removed the dropped knob")
    check(cm.param(group, USER_PAGE) is None, "and the page it created")

    # With a user page, built-in pages take no drops, so the knobs go onto the explicit page.
    gt.require(cm.add(group, "Page"), "clicked the Page palette entry on %s" % group)
    ok = yield ("until", lambda: cm.selected(group) == "page1", 5.0)
    gt.require(ok, "added page1 to %s" % group)
    gt.require(cm.show_page(group, "page1"), "showed page1")
    yield ("sleep", 300)

    gt.section("plain drop links")
    result = []
    gt.require(drag_label_onto(blur, overlay_end(group), Qt.NoModifier, Qt.NoModifier, result), "dragged %s's Size label" % blur)
    ok = yield ("until", lambda: cm.param(group, "size") is not None, 5.0)
    check(result[:1] == [True], "the Group's panel accepted the drop")
    check(ok, "the drop made a size knob on %s" % group)
    gt.shot("m373_drag_in_linked.png")
    if ok:
        set_value(src, 7.0)
        yield ("sleep", 300)
        check(abs(value(cm.param(group, "size")) - 7.0) < 1e-9, "the dropped knob follows %s.size" % blur)
        set_value(cm.param(group, "size"), 3.0)
        yield ("sleep", 300)
        check(abs(value(src) - 3.0) < 1e-9, "and %s.size follows it" % blur)

    gt.section("Ctrl-drop copies")
    gt.require(cm.show_page(group, "page1"), "showed page1")
    yield ("sleep", 300)
    result = []
    drag_label_onto(blur, overlay_end(group), Qt.NoModifier, Qt.ControlModifier, result)
    ok = yield ("until", lambda: cm.param(group, "size2") is not None, 5.0)
    check(ok, "the Ctrl-drop made size2 on %s" % group)
    if ok:
        copy = cm.param(group, "size2")
        check(abs(value(copy) - value(src)) < 1e-9, "the copy starts with %s.size's value" % blur)
        set_value(src, 11.0)
        yield ("sleep", 300)
        check(abs(value(cm.param(group, "size2")) - 11.0) > 1e-9, "the copy doesn't follow %s.size" % blur)
    gt.require(cm.undo(), "undo")
    ok = yield ("until", lambda: cm.param(group, "size2") is None, 5.0)
    check(ok, "one undo removed the copy")

    gt.section("Link knob...")
    inner = cm.natron_app().createNode("Blur", -1, cm.natron_app().getNode(group))
    gt.require(inner is not None, "created a Blur inside %s" % group)
    inner_name = inner.getScriptName()
    yield ("sleep", 300)
    gt.require(cm.invoke(group, "showLinkChooser"), "opened Link knob...")
    ok = yield ("until", lambda: cm.visible_child(group, QWidget, "KnobEditLinkChooser") is not None, 5.0)
    gt.require(ok, "the link chooser is shown")
    nodes = cm.visible_child(group, QComboBox, "KnobEditLinkNode")
    gt.require(nodes is not None, "the chooser has a node list")
    labels = [nodes.itemText(i) for i in range(nodes.count())]
    inner_path = "%s.%s" % (group, inner_name)
    index = [i for i in range(nodes.count()) if nodes.itemData(i) == inner_path]
    shown = [nodes.itemText(i) for i in range(nodes.count())]
    check(len(set(shown)) == len(shown), "the node list shows every node under a distinct text (%r)" % shown)
    check(bool(index) and nodes.itemText(index[0]) == inner_path, "the inner node is shown by its path (%r)" % shown)
    gt.require(bool(index), "the node list offers the inner %s (%r)" % (inner_name, labels))
    nodes.setCurrentIndex(index[0])
    nodes.activated.emit(index[0])
    QApplication.processEvents()
    yield ("sleep", 200)
    knobs = cm.visible_child(group, QComboBox, "KnobEditLinkKnob")
    gt.require(knobs is not None and knobs.findData("size") >= 0, "the knob list offers size")
    knobs.setCurrentIndex(knobs.findData("size"))
    gt.shot("m373_link_chooser.png")
    button = cm.visible_child(group, QAbstractButton, "KnobEditLinkButton")
    gt.require(button is not None, "the chooser has a Link button")
    button.click()
    QApplication.processEvents()
    ok = yield ("until", lambda: cm.param(group, "size2") is not None, 5.0)
    check(ok, "Link knob... made size2 (params %r)" % user_params(group))
    if ok:
        set_value(inner.getParam("size"), 5.0)
        yield ("sleep", 300)
        check(abs(value(cm.param(group, "size2")) - 5.0) < 1e-9, "size2 follows the inner node's size")

    gt.section("link drag outside edit mode")
    gt.require(cm.set_editing(group, False), "left edit mode")
    yield ("until", lambda: not cm.is_editing(group), 5.0)
    source = yield from cm.open_blur(0, 200, editing=False)
    src = cm.param(source, "size")
    other = yield from cm.open_blur(800, 0, editing=False)

    def other_label():
        w = cm.label_widget(other, SIZE_LABEL)
        return (w, QPoint(w.width() // 2, w.height() // 2)) if w is not None else None

    ok = yield ("until", lambda: cm.label_widget(source, SIZE_LABEL) is not None and other_label() is not None, 5.0)
    gt.require(ok, "%s and %s show their Size labels" % (source, other))
    result = []
    # Ctrl stays held through the drop: a drop without it does not link.
    gt.require(drag_label_onto(source, other_label, Qt.ControlModifier, Qt.ControlModifier, result),
               "Ctrl-dragged %s's Size label" % source)
    yield ("sleep", 400)
    check(bool(result), "the drag ran its drop on %s's Size label" % other)
    set_value(src, 13.0)
    yield ("sleep", 300)
    check(abs(value(cm.param(other, "size")) - 13.0) < 1e-9, "a Ctrl-drag between panels outside edit mode still links")
    check(cm.param(other, "size2") is None and not cm.is_editing(other), "and adds no user knob")


cm.start(steps())
