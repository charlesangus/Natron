"""Checks that the tab menu, read from the real Nodes menu through its adapter, lists no shader node
in the main graph, whatever the selection, and that a menu filtered for a selected network node
lists only node types that take a network.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m350_shader_tab_menu.py
"""
import json
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 100
MANIFEST_DIR = os.path.join(ROOT, "Engine", "Nodes", "Scene", "Shaders", "manifest")
RENDERERS = ("cycles", "moonray")
NETWORK_KIND = "network"

try:
    import faulthandler

    import guitest as gt
    from guitest import check, require

    import tabtabtab_natron

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def shader_ids():
        ids = set()
        for renderer in RENDERERS:
            with open(os.path.join(MANIFEST_DIR, renderer + ".json")) as f:
                ids.update(node["id"] for node in json.load(f)["nodes"])
        return ids

    def listed(context):
        """(plugin ID, input kinds) of every node item the menu lists for context."""
        items, infos = tabtabtab_natron._node_items()
        found = []
        for item, info in zip(items, infos):
            if not tabtabtab_natron._accepts_info(info, context):
                continue
            action = item["menuobj"]
            plugin_id = action.property("natronPluginID")
            kinds = [k for k in (action.property("natronInputKinds") or "").split(",") if k]
            found.append((plugin_id if isinstance(plugin_id, str) else "", kinds))
        return found

    def steps():
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        shaders = shader_ids()
        require(len(shaders) > 100, "the manifests list the shader nodes (%d)" % len(shaders))

        gt.section("outside a network")
        for context in ({}, {"selectedKind": "image"}, {"selectedKind": "scene"}, {"selectedKind": "deep"},
                        {"inputKind": "image"}, {"inputKind": "scene", "needsPrimPathKnob": True}):
            items = listed(context)
            check(len(items) > 0, "%r lists some nodes" % (context,))
            leaked = sorted(pid for pid, _kinds in items if pid in shaders)
            check(not leaked, "%r lists no shader node (got %r)" % (context, leaked[:5]))
            networked = sorted(pid for pid, kinds in items if NETWORK_KIND in kinds)
            check(not networked, "%r lists no node type taking a network (got %r)" % (context, networked[:5]))

        gt.section("inside a network")
        items = listed({"selectedKind": NETWORK_KIND})
        leaked = sorted(pid for pid, _kinds in items if pid in shaders)
        check(not leaked, "the Nodes menu holds no shader node to list (got %r)" % (leaked[:5],))
        others = sorted(pid for pid, kinds in items if pid and NETWORK_KIND not in kinds and "polymorphic" not in kinds)
        check(not others, "a selected network lists only node types that take one (got %r)" % (others[:5],))

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
