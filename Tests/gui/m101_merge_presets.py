"""Checks for the Merge presets that the bundled Gui/Resources/PyPlugs/initGui.py adds to the nodes
menu (NatronGui.natron.nodesMenu()) under Merge/.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m101-shots/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m101_merge_presets.py build/release/App/Natron

A build-tree Natron does not find the bundled PyPlugs, so m101_merge_presets.natronpath/initGui.py
runs the bundled script from the source tree; a bundle runs its own copy too, and the script adds
each preset once.

Sections:
  - menu: the nodes menu's Merge submenu holds one action per preset, in order, each carrying its
    operation as the natronMergePreset property;
  - trigger: triggering each preset creates one Merge node (plug-in ID "Merge") in the root graph
    with its operation set, and the node's sub-label shows that operation;
  - tab menu: the tab menu lists Plus under [Merge], and choosing it creates a Merge whose
    operation is plus.

Shots: m101-tab-menu-plus.png.
"""
import os
import re
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 280

MERGE_ID = "Merge"
MERGE_GROUPING = "Merge"
PRESET_PROPERTY = "natronMergePreset"
SUBLABEL_PARAM = "NatronOfxParamStringSublabelName"
PRESETS = [
    ("Plus", "plus"),
    ("Matte", "matte"),
    ("Multiply", "multiply"),
    ("In", "in"),
    ("Out", "out"),
    ("Screen", "screen"),
    ("Max", "max"),
    ("Min", "min"),
    ("Difference", "difference"),
]

try:
    import NatronGui
    from PySide6.QtCore import QPointF, Qt
    from PySide6.QtGui import QEnterEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLineEdit, QListView, QMainWindow

    import guitest as gt
    from guitest import check, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def merge_submenu():
        menu = NatronGui.natron.nodesMenu()
        for act in (menu.actions() if menu is not None else []):
            if act.property("natronGrouping") == MERGE_GROUPING and gt.menu_of(act) is not None:
                return gt.menu_of(act)
        return None

    def preset_actions():
        menu = merge_submenu()
        if menu is None:
            return []
        return [a for a in menu.actions() if a.property(PRESET_PROPERTY)]

    def root_names():
        return set(n.getScriptName() for n in app.getChildren())

    def new_nodes(before):
        return [n for n in app.getChildren() if n.getScriptName() not in before]

    def operation_of(node):
        param = node.getParam("operation")
        return param.getOption(param.getValue()) if param is not None else None

    def sublabel_of(node):
        param = node.getParam(SUBLABEL_PARAM)
        return param.getValue() if param is not None else None

    def enter_graph(view):
        centre = QPointF(view.rect().center())
        QApplication.sendEvent(view, QEnterEvent(centre, centre, QPointF(view.mapToGlobal(view.rect().center()))))

    def open_tab_menu(view):
        enter_graph(view)
        QTest.keyClick(view, Qt.Key_Tab)
        ok = yield ("until", lambda: any(d.isVisible() for d in gt.widgets_of("TabTabTabWidget")), POLL_TIMEOUT_S)
        if not ok:
            return None, None, None
        popup = next(d for d in gt.widgets_of("TabTabTabWidget") if d.isVisible())
        return popup, popup.findChildren(QLineEdit)[0], popup.findChildren(QListView)[0]

    def listed(line_edit, view, text):
        line_edit.setText(text)
        QApplication.processEvents()
        model = view.model()
        return [str(model.index(r, 0).data()) for r in range(model.rowCount())]

    def row_name(entry):
        return entry.split("[")[0].strip()

    def menu_section():
        gt.section("menu")
        ok = yield ("until", lambda: len(preset_actions()) >= len(PRESETS), POLL_TIMEOUT_S)
        require(ok, "the nodes menu's Merge submenu holds the presets",
                "found %r" % [a.text() for a in preset_actions()])
        actions = preset_actions()
        got = [(re.sub(r"&(&?)", r"\1", a.text()), a.property(PRESET_PROPERTY)) for a in actions]
        check(got == PRESETS, "menu: one action per preset, in order, carrying its operation (got %r)" % got)

    def trigger_section():
        gt.section("trigger")
        by_operation = dict((a.property(PRESET_PROPERTY), a) for a in preset_actions())
        for label, operation in PRESETS:
            action = by_operation.get(operation)
            if not check(action is not None, "trigger: %s has an action" % label):
                continue
            before = root_names()
            action.trigger()
            ok = yield ("until", lambda: len(new_nodes(before)) > 0, 5.0)
            created = new_nodes(before)
            if not check(ok and len(created) == 1, "trigger: %s creates one node in the root graph (got %d)"
                         % (label, len(created))):
                continue
            node = created[0]
            check(node.getPluginID() == MERGE_ID, "trigger: %s creates %s (got %s)" % (label, MERGE_ID, node.getPluginID()))
            check(operation_of(node) == operation, "trigger: %s sets operation %s (got %r)"
                  % (label, operation, operation_of(node)))
            check(sublabel_of(node) == operation, "trigger: %s's sub-label shows %s (got %r)"
                  % (label, operation, sublabel_of(node)))

    def tab_menu_section(main_graph):
        gt.section("tab menu")
        popup, line_edit, view = yield from open_tab_menu(main_graph)
        if not check(popup is not None, "tab menu: opens over the root graph"):
            return
        entries = listed(line_edit, view, "Plus")
        row = next((r for r, e in enumerate(entries) if row_name(e) == "Plus" and "[Merge]" in e), -1)
        check(row >= 0, "tab menu: lists Plus under [Merge] (got %r)" % entries)
        if row < 0:
            QTest.keyClick(popup, Qt.Key_Escape)
            QApplication.processEvents()
            return
        gt.shot("m101-tab-menu-plus.png")
        before = root_names()
        view.setCurrentIndex(view.model().index(row, 0))
        QTest.keyClick(line_edit, Qt.Key_Return)
        ok = yield ("until", lambda: len(new_nodes(before)) > 0, 5.0)
        created = new_nodes(before)
        if not check(ok and len(created) == 1, "tab menu: choosing Plus creates one node (got %d)" % len(created)):
            return
        check(created[0].getPluginID() == MERGE_ID and operation_of(created[0]) == "plus",
              "tab menu: the node is a Merge with operation plus (got %s, %r)"
              % (created[0].getPluginID(), operation_of(created[0])))

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        require(MERGE_ID in NatronGui.natron.getPluginIDs(), "the Merge node is registered", gt.plugin_problem(MERGE_ID))
        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active")
        gt.close_all_panels()

        main_graph = root_graph()
        require(main_graph is not None, "found the node graph view")
        main_graph.setFocus()
        QApplication.processEvents()

        yield from menu_section()
        yield from trigger_section()
        yield from tab_menu_section(main_graph)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
