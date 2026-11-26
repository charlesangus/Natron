"""A GUI test of the type-name search aliases of the shader tab menu: inside a USD Material Builder,
typing "float", "color", "vector" and their synonyms offers each target's constant nodes for the
type, and the rows still show the node's own name. The aliases come from the shader layout tables,
so the expectations are read from them. It is driven by Tests/gui/run-gui-test.sh (1920x1080), not
ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m351-gui/aliases GUI_TEST_TIMEOUT=300 \\
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_aliases.py <AppImage or build/fast/App/Natron>'

Shot: shader_graph_aliases.png (the menu filtered by "float").
"""
import json
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 500
MAX_SWITCHES = 8

# The nodes each type name must find, per renderer: the renderer's own constants for the type.
EXPECTED = {
    "cycles": {
        ("float", "scalar", "value", "int", "integer"): "cycles_value",
        ("color", "colour", "rgb"): "cycles_color",
        ("vector", "xyz"): "cycles_combine_xyz",
    },
    "moonray": {
        ("float", "scalar", "value", "int", "integer"): "ConstantScalarMap",
        ("color", "colour", "rgb"): "ConstantColorMap",
        ("vector", "xyz"): "UsdPrimvarReader_vector",
    },
}

try:
    import faulthandler

    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    import shader_graph_util as sg
    import tabtabtab_natron

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def layout_aliases(renderer):
        """{plugin ID: aliases} of the renderer's layout table."""
        with open(os.path.join(sg.MANIFEST_DIR, renderer + "_layout.json")) as f:
            nodes = json.load(f)["nodes"]
        return {node_id: tuple(entry.get("search_aliases", ())) for node_id, entry in nodes.items()}

    def rows_of(list_view):
        """(plugin ID, shown text, the node action's own name) of each row the menu lists."""
        items = getattr(list_view.model(), "_items", [])
        return [(item["menuobj"].property("natronPluginID"), item["text"], item["menuobj"].text().replace("&", ""))
                for item in items]

    def make_target_current(popup, line_edit, target):
        context = tabtabtab_natron._plugin.context() or {}
        for _ in range(MAX_SWITCHES):
            if tabtabtab_natron._network_target(context) == target:
                return True
            sg.key_click(line_edit, Qt.Key_Backtab, Qt.ShiftModifier)
            QApplication.processEvents()
        return tabtabtab_natron._network_target(context) == target

    def steps():
        if app.getNode("Viewer1") is None:
            app.createNode("Viewer")
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        sg.park_viewer(app)
        require(ok, "the main window is up")

        gt.section("builder graph")
        builder = sg.create(app, sg.BUILDER, 0, 0)
        yield ("sleep", SETTLE_MS)
        require(sg.open_graph(app, builder), "the builder's graph has a tab")
        ok = yield ("until", lambda: sg.graph_of(builder) is not None and sg.graph_of(builder).isVisible(), 10.0)
        require(ok, "the builder's graph is shown")
        graph = sg.graph_of(builder)

        popup, line_edit, list_view = yield from sg.open_tab_menu(graph)
        require(popup is not None, "Tab opens the menu in the builder's graph")
        context = tabtabtab_natron._plugin.context() or {}
        targets = tuple(context.get("networkTargets") or ())
        for renderer in sg.RENDERERS:
            require(renderer in targets, "the builder's targets include %s (%r)" % (renderer, targets))

        for renderer in sg.RENDERERS:
            gt.section(renderer)
            aliases = layout_aliases(renderer)
            require(make_target_current(popup, line_edit, renderer), "Shift+Tab makes %s the current target" % renderer)
            others = set(pid for r in sg.RENDERERS if r != renderer for pid in layout_aliases(r))
            for words, plugin_id in EXPECTED[renderer].items():
                check(set(words) <= set(aliases.get(plugin_id, ())),
                      "the %s layout table lists %s for %s" % (renderer, words, plugin_id))
                for word in words:
                    line_edit.setText(word)
                    QApplication.processEvents()
                    rows = rows_of(list_view)
                    ids = [pid for pid, _text, _name in rows]
                    check(plugin_id in ids, "typing %r offers %s (got %r)" % (word, plugin_id, ids[:6]))
                    check(not any(pid in others for pid in ids), "typing %r offers nothing of the other target" % word)
                    shown = [(text, name) for pid, text, name in rows if pid == plugin_id]
                    if shown:
                        check(shown[0][0].startswith(shown[0][1] + " ["),
                              "the row for %s shows the node's own name %r (got %r)" % (plugin_id, shown[0][1], shown[0][0]))
                if renderer == "cycles" and "float" in words:
                    line_edit.setText("float")
                    QApplication.processEvents()
                    gt.shot("shader_graph_aliases.png")
            line_edit.setText("")
            QApplication.processEvents()

        sg.close_tab_menu(popup)
        yield ("sleep", SETTLE_MS)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
