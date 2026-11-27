"""A GUI test of the knob editor's property section: renaming a user float rewrites the expression
another node has on it, range and tooltip edits commit, an animated Int turns into a Float with its
keys, a duplicate name shows an inline error and changes nothing, a button's script runs when the
button is clicked, and each edit is one undo step. Driven by Tests/gui/run-gui-test.sh.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m373-properties/out GUI_TEST_TIMEOUT=240 \\
        Tests/gui/run-gui-test.sh Tests/gui/m373_properties.py <AppImage or build/fast/App/Natron>'
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

import guitest as gt
from guitest import check
import knob_edit_common as cm

from PySide6.QtCore import QEvent, Qt
from PySide6.QtGui import QFocusEvent
from PySide6.QtWidgets import QApplication, QComboBox, QPlainTextEdit


def expression_of(node, knob):
    p = cm.param(node, knob)
    if p is None:
        return ""
    e = p.getExpression(0)
    return e[0] if isinstance(e, (tuple, list)) else (e or "")


def choose_type(node, entry_id):
    combo = cm.visible_child(node, QComboBox, "KnobEditPropertiesType")
    if combo is None:
        return False
    index = combo.findData(entry_id)
    if index < 0:
        return False
    combo.setCurrentIndex(index)
    combo.activated.emit(index)
    QApplication.processEvents()
    return True


def steps():
    yield from cm.wait_for_graph()
    a = yield from cm.open_blur()
    b = cm.create("Blur", 400, 0)
    cm.add(a, "Float")
    ok = yield ("until", lambda: cm.selected(a) == "float1", 5.0)
    gt.require(ok, "added float1 to %s" % a)
    size = cm.param(b, "size")
    gt.require(size is not None and size.setExpression("%s.float1.get()" % a, False, 0), "%s.size expresses on float1" % b)
    yield ("sleep", 300)
    check("Float 1" in cm.property_title(a) and "Float" in cm.property_title(a),
          "the section title names the knob and its type (%r)" % cm.property_title(a))
    gt.shot("m373_properties_float.png")

    gt.section("rename")
    gt.require(cm.commit_line(a, "KnobEditPropertiesName", "gain"), "typed gain into the script name")
    ok = yield ("until", lambda: cm.param(a, "gain") is not None, 5.0)
    check(ok and cm.param(a, "float1") is None, "float1 was renamed gain")
    check(ok and cm.param(a, "gain").getLabel() == "Gain", "a default label follows the rename (%r)" %
          (cm.param(a, "gain").getLabel() if ok else None,))
    check("gain" in expression_of(b, "size") and "float1" not in expression_of(b, "size"),
          "%s.size's expression was rewritten (%r)" % (b, expression_of(b, "size")))
    check(cm.selected(a) == "gain", "the renamed knob stays selected")

    gt.section("range and tooltip")
    yield ("sleep", 200)
    gt.require(cm.commit_line(a, "KnobEditPropertiesRangeMax", "10"), "typed a maximum")
    ok = yield ("until", lambda: abs(cm.param(a, "gain").getMaximum(0) - 10.0) < 1e-9, 5.0)
    check(ok, "the maximum is 10")
    yield ("sleep", 200)
    gt.require(cm.commit_line(a, "KnobEditPropertiesTooltip", "How much gain."), "typed a tooltip")
    ok = yield ("until", lambda: cm.param(a, "gain").getHelp() == "How much gain.", 5.0)
    check(ok, "the tooltip was set")

    gt.section("duplicate name")
    yield ("sleep", 200)
    cm.commit_line(a, "KnobEditPropertiesName", "size")
    ok = yield ("until", lambda: bool(cm.property_error(a)), 3.0)
    check(ok, "a duplicate name shows an inline error (%r)" % cm.property_error(a))
    check(cm.param(a, "gain") is not None and cm.param(a, "gain").getScriptName() == "gain", "nothing was renamed")
    gt.shot("m373_properties_error.png")

    gt.section("type change")
    cm.add(a, "Int")
    ok = yield ("until", lambda: cm.selected(a) == "int1", 5.0)
    gt.require(ok, "added int1")
    i = cm.param(a, "int1")
    i.setValueAtTime(3, 1, 0)
    i.setValueAtTime(9, 10, 0)
    yield ("sleep", 300)
    gt.require(choose_type(a, "Float"), "chose Float in the type combo")
    ok = yield ("until", lambda: cm.param(a, "int1") is not None and cm.param(a, "int1").getTypeName() == "Double", 5.0)
    check(ok, "int1 is now a Float")
    if ok:
        f = cm.param(a, "int1")
        check(f.getNumKeys(0) == 2, "the keys were kept (%d)" % f.getNumKeys(0))
        check(abs(f.getValueAtTime(10, 0) - 9.0) < 1e-9, "the key at frame 10 is still 9")
    check(not cm.property_error(a), "no error after the conversion")

    gt.section("button script")
    cm.add(a, "Button")
    ok = yield ("until", lambda: cm.selected(a) == "button1", 5.0)
    gt.require(ok, "added button1")
    yield ("sleep", 300)
    gt.shot("m373_properties_button.png")
    script = cm.visible_child(a, QPlainTextEdit, "KnobEditPropertiesScript")
    gt.require(script is not None, "the button has a script editor")
    script.setPlainText("import NatronEngine\nNatronEngine.natron.getInstance(0).getNode('%s').getParam('gain').set(0.25)\n" % a)
    QApplication.sendEvent(script, QFocusEvent(QEvent.FocusOut, Qt.OtherFocusReason))
    QApplication.processEvents()
    yield ("sleep", 400)
    gt.require(cm.set_editing(a, False), "left edit mode")
    yield ("until", lambda: not cm.is_editing(a), 5.0)
    button = cm.label_widget(a, "Button 1")
    gt.require(button is not None and hasattr(button, "click"), "found the Button 1 push button")
    button.click()
    ok = yield ("until", lambda: abs(cm.param(a, "gain").getValue(0) - 0.25) < 1e-9, 5.0)
    check(ok, "clicking the button ran its script")
    gt.require(cm.set_editing(a, True), "back in edit mode")
    yield ("until", lambda: cm.is_editing(a), 5.0)

    gt.section("undo")
    # The button's edits come first: its creation, its script and, when a button's Python edits
    # are recorded, the value it set; at most three steps remove them.
    for _ in range(3):
        if cm.param(a, "button1") is None:
            break
        gt.require(cm.undo(), "undo a button edit")
        yield ("sleep", 300)
    check(cm.param(a, "button1") is None, "button1 is gone after undoing its edits")
    gt.require(cm.undo(), "undo the type change")
    ok = yield ("until", lambda: cm.param(a, "int1") is not None and cm.param(a, "int1").getTypeName() == "Int", 5.0)
    check(ok, "one undo turned int1 back into an Int")
    gt.require(cm.undo(), "undo adding int1")
    yield ("until", lambda: cm.param(a, "int1") is None, 5.0)
    gt.require(cm.undo(), "undo the tooltip")
    ok = yield ("until", lambda: cm.param(a, "gain").getHelp() == "", 5.0)
    check(ok, "one undo cleared the tooltip")
    gt.require(cm.undo(), "undo the maximum")
    ok = yield ("until", lambda: abs(cm.param(a, "gain").getMaximum(0) - 10.0) > 1e-9, 5.0)
    check(ok, "one undo restored the maximum")
    gt.require(cm.undo(), "undo the rename")
    ok = yield ("until", lambda: cm.param(a, "float1") is not None, 5.0)
    check(ok, "one undo renamed gain back to float1")
    check("float1" in expression_of(b, "size"), "and %s.size's expression with it (%r)" % (b, expression_of(b, "size")))


cm.start(steps())
