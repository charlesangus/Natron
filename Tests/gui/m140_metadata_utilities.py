"""Checks for the native metadata nodes in the GUI.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m140-shots/out -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m140_metadata_utilities.py build/release/App/Natron

Sections:
  - tab menu: the Metadata folder of natron.nodesMenu(), the tab menu's node source, holds exactly
    the seven native metadata nodes and no action for an org.openfx.examples.metadata* plug-in, and
    none of those IDs is a registered plug-in;
  - compare: a CompareMetadata over two Reads of one file lists no difference; after a ModifyMetadata
    on B's branch sets a key, the Differences box reports that key as B only, and the viewer shows
    the CompareMetadata.

Shots: m140-compare-differences.png (the CompareMetadata panel and the viewer).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
WATCHDOG_S = 280

NATIVE_METADATA_IDS = {
    "fr.natron.CompareMetadata", "fr.natron.ContributeMetadata", "fr.natron.CopyMetadata",
    "fr.natron.ModifyMetadata", "fr.natron.RemoveMetadata", "fr.natron.TimeCodeMetadata",
    "fr.natron.ViewMetadata",
}
OFX_EXAMPLE_PREFIX = "org.openfx.examples.metadata"
METADATA_GROUPING = "Metadata"
COMPARE_ID = "fr.natron.CompareMetadata"
MODIFY_ID = "fr.natron.ModifyMetadata"
VIEWER_ID = "fr.inria.built-in.Viewer"
FIXTURE = "png-8bit.png"
TEST_KEY = "m140.difference"
TEST_VALUE = "present on B only"

try:
    import NatronEngine
    import NatronGui
    from PySide6.QtCore import QMetaObject
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return panel
                except RuntimeError:
                    continue
        return None

    def metadata_folder():
        menu = NatronGui.natron.nodesMenu()
        for act in (menu.actions() if menu is not None else []):
            if act.property("natronGrouping") == METADATA_GROUPING and gt.menu_of(act) is not None:
                return gt.menu_of(act)
        return None

    def leaf_actions(menu):
        found = []
        for act in menu.actions():
            sub = gt.menu_of(act)
            if sub is not None:
                found.extend(leaf_actions(sub))
            elif act.text():
                found.append(act)
        return found

    def tab_menu_section():
        gt.section("tab menu")
        folder = metadata_folder()
        require(folder is not None, "the nodes menu has a Metadata folder")
        actions = leaf_actions(folder)
        ids = sorted(str(a.property("natronPluginID")) for a in actions)
        report("NOTE Metadata folder: %r" % [(a.text(), a.property("natronPluginID")) for a in actions])
        check(set(ids) == NATIVE_METADATA_IDS and len(ids) == len(NATIVE_METADATA_IDS),
              "the Metadata folder lists exactly the seven native metadata nodes (got %r)" % ids)
        check(not any(i.startswith(OFX_EXAMPLE_PREFIX) for i in ids),
              "the Metadata folder lists no OFX example metadata node")
        everywhere = [str(a.property("natronPluginID")) for a in leaf_actions(NatronGui.natron.nodesMenu())]
        check(not any(i.startswith(OFX_EXAMPLE_PREFIX) for i in everywhere),
              "no action anywhere in the nodes menu is an OFX example metadata node")
        registered = [i for i in NatronEngine.natron.getPluginIDs() if i.startswith(OFX_EXAMPLE_PREFIX)]
        check(not registered, "no org.openfx.examples.metadata* plug-in is registered (%r)" % registered)
        missing = sorted(NATIVE_METADATA_IDS - set(NatronEngine.natron.getPluginIDs()))
        check(not missing, "all seven native metadata nodes are registered (missing %r)" % missing)

    def differences(compare):
        param = compare.getParam("differences")
        return param.getValue() if param is not None else None

    def compare_section():
        gt.section("compare")
        path = os.path.join(gt.FIXTURES, FIXTURE)
        require(os.path.isfile(path), "fixture exists: " + FIXTURE)

        read_a = app.createReader(path)
        read_b = app.createReader(path)
        require(read_a is not None and read_b is not None, "create two Reads of " + FIXTURE)
        read_a.setPosition(0, 0)
        read_b.setPosition(300, 0)

        modify = app.createNode(MODIFY_ID)
        require(modify is not None, "create ModifyMetadata", gt.plugin_problem(MODIFY_ID))
        modify.setPosition(300, 120)
        require(modify.connectInput(0, read_b), "connect Read B to ModifyMetadata")

        compare = app.createNode(COMPARE_ID)
        require(compare is not None, "create CompareMetadata", gt.plugin_problem(COMPARE_ID))
        compare.setPosition(150, 260)
        require(compare.connectInput(0, read_a), "connect Read A to CompareMetadata's A")
        require(compare.connectInput(1, modify), "connect ModifyMetadata to CompareMetadata's B")

        viewer = app.getNode("Viewer1") or app.createNode(VIEWER_ID)
        require(viewer is not None, "a Viewer node exists")
        viewer.setPosition(150, 400)
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, compare), "connect the Viewer to CompareMetadata")

        require(compare.getParam("differences") is not None, "CompareMetadata has a differences param")
        ok = yield ("until", gt.viewer_up, POLL_TIMEOUT_S)
        check(ok, "the viewer is up")
        yield ("sleep", 1000)

        before = differences(compare)
        report("NOTE differences with no operation set: %r" % before)
        check(before == "", "two Reads of one file show no differences (%r)" % before)

        for name, value in (("operation1", 1), ("type1", 2)):
            param = modify.getParam(name)
            require(param is not None, "ModifyMetadata has a " + name + " param")
            param.setValue(value)
        for name, value in (("key1", TEST_KEY), ("stringValue1", TEST_VALUE)):
            param = modify.getParam(name)
            require(param is not None, "ModifyMetadata has a " + name + " param")
            param.setValue(value)

        ok = yield ("until", lambda: bool(differences(compare)), POLL_TIMEOUT_S)
        if not ok:
            compare.getParam("refresh").trigger()
            ok = yield ("until", lambda: bool(differences(compare)), POLL_TIMEOUT_S)
        text = differences(compare)
        report("NOTE differences after Set: %r" % text)
        check(ok, "the Differences box fills in after ModifyMetadata sets a key")
        check(text is not None and "B only: " + TEST_KEY in text and TEST_VALUE in text,
              "the Differences box reports the key as B only with its value (%r)" % text)
        check(text is not None and len(text.strip().splitlines()) == 1,
              "the key is the only difference (%r)" % text)

        app.selectNode(compare, True)
        QMetaObject.invokeMethod(node_graph_view(), "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(compare) is not None, POLL_TIMEOUT_S)
        check(ok, "open CompareMetadata's settings panel")
        yield ("sleep", 800)
        check(gt.viewer_up(), "the viewer is showing")
        gt.shot("m140-compare-differences.png")

    def steps():
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        gt.close_all_panels()
        tab_menu_section()
        yield from compare_section()
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
