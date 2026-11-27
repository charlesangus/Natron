"""A GUI test of the knob editor's palette: clicking each entry adds a knob of that type after the
selection with a unique name, selects it and focuses its script name, and one Ctrl+Z removes the
last one. Driven by Tests/gui/run-gui-test.sh.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m373-palette/out GUI_TEST_TIMEOUT=240 \\
        Tests/gui/run-gui-test.sh Tests/gui/m373_palette.py <AppImage or build/fast/App/Natron>'
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

import guitest as gt
from guitest import check
import knob_edit_common as cm

from PySide6.QtWidgets import QApplication, QLineEdit

# Palette entry, script name of the first knob it adds, its type name and dimension.
ENTRIES = [
    ("Int", "int1", "Int", 1),
    ("Float", "float1", "Double", 1),
    ("Bool", "bool1", "Bool", 1),
    ("Choice", "choice1", "Choice", 1),
    ("Color", "color1", "Color", 3),
    ("String", "string1", "String", 1),
    ("File", "file1", "InputFile", 1),
    ("OutputFile", "outputFile1", "OutputFile", 1),
    ("Path", "path1", "Path", 1),
    ("Button", "button1", "Button", 1),
    ("Curve", "curve1", "Parametric", 1),
    ("Layer", "layer1", "LayerSelect", 1),
    ("ChannelSet", "channelSet1", "ChannelSet", 1),
    ("ChannelSelect", "channelSelect1", "ChannelSelect", 1),
    ("Group", "group1", "Group", 1),
    ("Separator", "separator1", "Separator", 1),
    ("Label", "label1", "String", 1),
    ("Page", "page1", "Page", 1),
]


def steps():
    yield from cm.wait_for_graph()
    blur = yield from cm.open_blur()
    gt.shot("m373_palette.png")

    gt.section("click to add")
    previous = None
    for entry, name, type_name, dim in ENTRIES:
        button = cm.palette_button(blur, entry)
        if not check(button is not None and button.isVisible(), "the palette has a visible %s entry" % entry):
            continue
        button.click()
        QApplication.processEvents()
        ok = yield ("until", lambda: cm.param(blur, name) is not None and cm.selected(blur) == name, 5.0)
        if not check(ok, "%s added %s and selected it (selected %r)" % (entry, name, cm.selected(blur))):
            continue
        p = cm.param(blur, name)
        check(p.getTypeName() == type_name, "%s is a %s (got %s)" % (name, type_name, p.getTypeName()))
        check(p.getNumDimensions() == dim, "%s has %d dimension(s)" % (name, dim))
        check(p.getLabel() != "" and p.getLabel()[0].isupper(), "%s has a readable label (%r)" % (name, p.getLabel()))
        ok = yield ("until", lambda: cm.visible_child(blur, QLineEdit, "KnobEditPropertiesName") is not None and
                    cm.visible_child(blur, QLineEdit, "KnobEditPropertiesName").hasFocus(), 3.0)
        check(ok, "the script name field has the focus after adding %s" % name)
        if previous is not None and type_name != "Page":
            parent = p.getParent()
            if parent is not None:
                siblings = cm.child_names(blur, parent.getScriptName())
                if previous in siblings and name in siblings:
                    check(siblings.index(name) == siblings.index(previous) + 1,
                          "%s went right after the selected %s (%r)" % (name, previous, siblings))
        previous = name
    check("separator1" not in cm.child_names(blur, "group1"),
          "a knob added while a group is selected goes after the group, not into it")
    yield ("sleep", 300)
    gt.shot("m373_palette_all_types.png")

    gt.section("names stay unique")
    cm.add(blur, "Float")
    ok = yield ("until", lambda: cm.param(blur, "float2") is not None, 5.0)
    check(ok, "a second Float is named float2")

    gt.section("undo")
    gt.require(cm.undo(), "undo")
    ok = yield ("until", lambda: cm.param(blur, "float2") is None, 5.0)
    check(ok, "one Ctrl+Z removed the last added knob")
    check(cm.param(blur, "float1") is not None and cm.param(blur, "page1") is not None,
          "the earlier knobs are still there")


cm.start(steps())
