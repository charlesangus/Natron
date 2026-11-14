"""Deferred matching on a heavy payload city: menus and Centre Pivot stay off the main thread.

Generates a city of NATRON_M381_BLOCKS x NATRON_M381_PER_BLOCK payload buildings (default 20 x 20, each
a few dozen prims, one authoring a "style" variant set) under $NATRON_GUI_TEST_OUT, with a camera
outside every payload, and builds USDRead -> USDVariant -> USDRenderSettings -> USDEditTransform ->
Viewer. Nothing is loaded in the viewer. Checks:
  - with the USDRenderSettings panel open, and then the USDVariant panel, editing USDVariant's
    pattern and filling the menus it moves keeps the main thread's worst gap within STALL_LIMIT_MS;
    both USDVariant menus fill with a disabled "N unloaded branches not searched" entry, the camera
    menu lists the camera, and no building's layer is opened (nor does the viewer record a load edit);
  - loading one building in the viewer (Scene Graph panel) refills the Variant Sets menu with that
    building's variant set and the Variants menu with its variants, within the stall budget from the
    load's commit on;
  - USDEditTransform's Centre Pivot button changes the pivot to that building's centre, within the
    stall budget; two presses in a row at different frames leave the pivot from the second.

The main thread's gaps are measured by a 10 ms heartbeat timer over each window, less the time the
thread was ready to run without a CPU. Creating a panel's widgets and looking widgets up from the
script both take the main thread far longer than the budget and are not what the windows measure,
so panels are opened and widgets found before a window starts, and the polls inside one read
knobs and held widgets only. With NATRON_STALL_LOG set, StallMonitor's log is also checked for
stalls starting inside the window.
Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import json
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
STARTUP_TIMEOUT_S = 120.0
SETTLE_MS = 600
HEARTBEAT_MS = 10
STALL_LIMIT_MS = 50.0
BLOCKS = int(os.environ.get("NATRON_M381_BLOCKS", "20"))
PER_BLOCK = int(os.environ.get("NATRON_M381_PER_BLOCK", "20"))
FILLERS = 24
STALL_LOG = os.environ.get("NATRON_STALL_LOG", "")

WORLD = "/World"
BLOCK0 = "/World/Block0"
TARGET = BLOCK0 + "/B0"
CAMERA = "/World/Cam"
UNSEARCHED_ID = "<unsearched>"
UNSEARCHED_TEXT = "unloaded branches not searched"
VARIANT_SET = "style"
VARIANTS = ("day", "night")
FRAME_A = 1
FRAME_B = 10
# The target's Floor translates from START to END over frames FRAME_A..FRAME_B, so Centre Pivot at
# FRAME_A reads x = 3 and at FRAME_B x = 13.
START_X = 3.0
END_X = 13.0
STATE_UNLOADED, STATE_LOADED = 1, 3


def write_city(directory):
    """Writes the root layer and every building's payload file; returns the root layer's path."""
    payloads = os.path.join(directory, "payloads")
    os.makedirs(payloads, exist_ok=True)
    body = ['#usda 1.0\n(\n    defaultPrim = "Building"\n)\n'
            'def Xform "Building" (\n    prepend variantSets = "%s"\n    variants = {\n        string %s = "%s"\n    }\n)\n{\n'
            % (VARIANT_SET, VARIANT_SET, VARIANTS[0]),
            '    variantSet "%s" = {\n' % VARIANT_SET]
    for v in VARIANTS:
        body.append('        "%s" {\n        }\n' % v)
    body.append('    }\n    def Xform "Floor"\n    {\n'
                '        double3 xformOp:translate.timeSamples = {\n            %d: (%g, 1, 5),\n            %d: (%g, 1, 5),\n        }\n'
                '        uniform token[] xformOpOrder = ["xformOp:translate"]\n'
                '        def Cube "Geo"\n        {\n            double size = 1\n        }\n' % (FRAME_A, START_X, FRAME_B, END_X))
    for k in range(FILLERS):
        body.append('        def Cube "Fill%d"\n        {\n            double size = 0.05\n'
                    '            double3 xformOp:translate = (%g, %g, %g)\n'
                    '            uniform token[] xformOpOrder = ["xformOp:translate"]\n        }\n'
                    % (k, 0.3 * ((k % 3) - 1), 0.3 * (((k // 3) % 3) - 1), 0.3 * (((k // 9) % 3) - 1)))
    body.append('    }\n}\n')
    with open(os.path.join(payloads, "building.usda"), "w") as f:
        f.write("".join(body))

    path = os.path.join(directory, "m381_city.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "World"\n    upAxis = "Y"\n    startTimeCode = %d\n    endTimeCode = %d\n)\n'
                'def Xform "World"\n{\n    def Camera "Cam"\n    {\n    }\n' % (FRAME_A, FRAME_B))
        for b in range(BLOCKS):
            f.write('    def Xform "Block%d"\n    {\n' % b)
            for i in range(PER_BLOCK):
                f.write('        def Xform "B%d" (\n            prepend payload = @./payloads/building.usda@\n        )\n        {\n        }\n' % i)
            f.write('    }\n')
        f.write('}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import QMetaObject, QModelIndex, QTimer, Qt
    from PySide6.QtWidgets import QApplication, QLineEdit, QLabel, QTreeView
    from pxr import Sdf

    from guitest import OUT, check, close_all_panels, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1
    STATE_ROLE = Qt.UserRole + 3

    fixture_dir = os.path.realpath(os.path.join(OUT, "m381_deferred_matching"))
    scene_path = write_city(fixture_dir)
    payload_dir = os.path.join(fixture_dir, "payloads")

    def make(plugin, upstream):
        node = app.createNode(plugin)
        require(node is not None, "create " + plugin, plugin_problem(plugin))
        if upstream is not None:
            require(node.connectInput(0, upstream), "connect %s to its upstream" % plugin)
        return node

    read = make("USDRead", None)
    read.getParam("filename").setValue(scene_path)
    variant = make("USDVariant", read)
    settings = make("USDRenderSettings", variant)
    edit = make("USDEditTransform", settings)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def main_thread_waited_ms():
        """The calling thread's total time ready to run without a CPU."""
        with open("/proc/thread-self/schedstat") as f:
            return int(f.read().split()[1]) / 1000000.0

    # The main thread's gaps over a window, from a heartbeat timer, and StallMonitor's log if set.
    # A gap leaves out the time the thread waited for a CPU, which a host busy rendering adds.
    class Window(object):
        def __init__(self):
            self.timer = QTimer()
            self.timer.setInterval(HEARTBEAT_MS)
            self.timer.timeout.connect(self.tick)
            self.worst = 0.0
            self.worst_wall = 0.0
            self.begin = 0.0
            self.last = 0.0
            self.waited = 0.0

        def tick(self):
            now = time.monotonic()
            waited = main_thread_waited_ms()
            wall = (now - self.last) * 1000.0 - HEARTBEAT_MS
            self.worst_wall = max(self.worst_wall, wall)
            self.worst = max(self.worst, wall - (waited - self.waited))
            self.last = now
            self.waited = waited

        def start(self):
            self.worst = 0.0
            self.worst_wall = 0.0
            self.begin = self.last = time.monotonic()
            self.waited = main_thread_waited_ms()
            self.timer.start()

        def stop(self):
            self.tick()
            self.timer.stop()
            stalls = []
            if STALL_LOG and os.path.isfile(STALL_LOG):
                with open(STALL_LOG) as f:
                    for raw in f:
                        try:
                            s = json.loads(raw)
                            if float(s["start_ms"]) >= self.begin * 1000.0 and float(s["duration_ms"]) > STALL_LIMIT_MS:
                                stalls.append((float(s["start_ms"]) - self.begin * 1000.0, float(s["duration_ms"])))
                        except (KeyError, TypeError, ValueError):
                            continue
            return self.worst, stalls

    window = Window()

    def end_window(label):
        worst, stalls = window.stop()
        report("NOTE %s: worst main-thread gap %.1f ms, %.1f ms with waiting for a CPU (budget %.0f ms)%s" %
               (label, worst, window.worst_wall, STALL_LIMIT_MS, "" if STALL_LOG else "; NATRON_STALL_LOG not set, heartbeat only"))
        check(worst <= STALL_LIMIT_MS, "%s: the main thread's worst gap %.1f ms stays within %.0f ms" % (label, worst, STALL_LIMIT_MS))
        check(not stalls, "%s: StallMonitor logs no stall over %.0f ms (got %r)" % (label, STALL_LIMIT_MS, stalls[:5]))

    # Looking a widget up wraps every widget in the application, which takes the main thread far
    # longer than the stall budget. The polls inside a window run there too, so they reuse a widget
    # found once while it stays shown.
    _found = {}

    def found(key, lookup):
        w = _found.get(key)
        try:
            if w is not None and w.isVisible():
                return w
        except RuntimeError:
            pass
        w = lookup()
        _found[key] = hold(w) if w is not None else None
        return w

    def view():
        def lookup():
            views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
            return views[0] if views else None
        return found("view", lookup)

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def load_edits():
        return view().property("natronSceneLoadEdits")

    def loads_quiet():
        return not view().property("natronSceneLoadsPending")

    def opened_buildings():
        """The payload files of the fixture that are open in the process."""
        return sorted(os.path.basename(l.realPath or "") for l in Sdf.Layer.GetLoadedLayers()
                      if (l.realPath or "").startswith(payload_dir))

    def options(node, name):
        return [str(o) for o in node.getParam(name).getOptions()]

    def combo_texts(node, name):
        """The item labels of the panel combo that holds the knob's entries, found by what it lists."""
        wanted = options(node, name)
        for w in widgets_of("ComboBox"):
            try:
                items = [str(i) for i in (w.property("natronItems") or [])]
            except RuntimeError:
                continue
            if w.isVisible() and len(items) == len(wanted) and (UNSEARCHED_ID in wanted) == any(UNSEARCHED_TEXT in i for i in items):
                return hold(w), items
        return None, []

    def main_gui():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        return hold(guis[0]) if guis else None

    def graph_view():
        views = [v for v in widgets_of("NodeGraph") if v.isVisible()]
        return hold(views[0]) if views else None

    def node_panel(node):
        label = node.getLabel()
        for panel in widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return hold(panel)
                except RuntimeError:
                    continue
            if label in (panel.windowTitle() or ""):
                return hold(panel)
        return None

    def open_panel(node):
        """Steps that open node's settings panel on its own, retrying once; returns whether it opened.
        showSelectedNodeSettingsPanel does nothing unless node is the only selected node."""
        for attempt in range(2):
            close_all_panels()
            yield ("sleep", 300)
            app.clearSelection()
            gui = main_gui()
            if gui is not None:
                gui.activateWindow()
                gui.raise_()
            app.selectNode(node, True)
            QApplication.processEvents()
            gv = graph_view()
            if gv is not None:
                QMetaObject.invokeMethod(gv, "showSelectedNodeSettingsPanel")
            ok = yield ("until", lambda: node_panel(node) is not None, POLL_TIMEOUT_S / 2)
            if ok:
                return True
            report("NOTE %s's settings panel did not open (attempt %d)" % (node.getScriptName(), attempt + 1))
        return False

    # The Scene Graph panel, to load a building in the viewer.
    def panel():
        def lookup():
            panels = [w for w in widgets_of("SceneGraphPanel") if w.isVisible()]
            return panels[0] if panels else None
        return found("panel", lookup)

    def tree():
        def lookup():
            p = panel()
            return p.findChild(QTreeView, "sceneGraphTree") if p is not None else None
        return found("tree", lookup)

    def rows(parent=QModelIndex()):
        m = tree().model()
        while m.canFetchMore(parent):
            m.fetchMore(parent)
        return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]

    def find(path):
        parent = QModelIndex()
        found = QModelIndex()
        for depth in range(1, path.count("/") + 1):
            prefix = "/" + "/".join(path.strip("/").split("/")[:depth])
            found = QModelIndex()
            for index in rows(parent):
                if str(index.data(PATH_ROLE)) == prefix:
                    found = index
                    break
            if not found.isValid():
                return found
            parent = found
        return found

    def state(path):
        index = find(path)
        return index.data(STATE_ROLE) if index.isValid() else None

    def pivot():
        p = edit.getParam("pivot")
        return tuple(float(p.getValue(d)) for d in range(3))

    def steps():
        require(viewer.connectInput(0, edit), "connect the Viewer to USDEditTransform")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        gui = main_gui()
        if gui is not None:
            gui.resize(1920, 1080)
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        require(ok, "the scene view shows the city (%d buildings)" % (BLOCKS * PER_BLOCK))
        check(opened_buildings() == [], "opening the city opens no building's layer (got %d)" % len(opened_buildings()))
        check(load_edits() == 0, "the viewer starts with nothing loaded (load edits %r)" % (load_edits(),))
        yield ("sleep", SETTLE_MS)

        # 1. Properties panels and menus with nothing loaded. USDRenderSettings is downstream of
        # USDVariant, so an edit of USDVariant's pattern refreshes the camera menu too.
        section("menus with nothing loaded")
        variant.getParam("variantSet").setValue(VARIANT_SET)
        edit.getParam("primPattern").setValue(TARGET)
        opened = yield from open_panel(settings)
        check(opened, "the USDRenderSettings panel opens")
        yield ("sleep", SETTLE_MS)
        window.start()
        variant.getParam("primPattern").setValue(TARGET)
        ok = yield ("until", lambda: UNSEARCHED_ID in options(variant, "variantSets") and UNSEARCHED_ID in options(variant, "variants"),
                    POLL_TIMEOUT_S)
        check(ok, "both USDVariant menus show the unsearched entry (sets %r, variants %r)" %
              (options(variant, "variantSets"), options(variant, "variants")))
        ok = yield ("until", lambda: CAMERA in options(settings, "camera"), POLL_TIMEOUT_S)
        check(ok, "the camera menu lists %s (got %r)" % (CAMERA, options(settings, "camera")))
        yield ("sleep", SETTLE_MS)
        end_window("editing the pattern and filling the menus with the USDRenderSettings panel open")

        opened = yield from open_panel(variant)
        check(opened, "the USDVariant panel opens")
        yield ("sleep", SETTLE_MS)
        window.start()
        variant.getParam("primPattern").setValue("")
        ok = yield ("until", lambda: UNSEARCHED_ID not in options(variant, "variantSets"), POLL_TIMEOUT_S)
        check(ok, "a blank pattern empties the Variant Sets menu (got %r)" % (options(variant, "variantSets"),))
        variant.getParam("primPattern").setValue(TARGET)
        ok = yield ("until", lambda: UNSEARCHED_ID in options(variant, "variantSets") and UNSEARCHED_ID in options(variant, "variants"),
                    POLL_TIMEOUT_S)
        check(ok, "both USDVariant menus show the unsearched entry again (sets %r, variants %r)" %
              (options(variant, "variantSets"), options(variant, "variants")))
        yield ("sleep", SETTLE_MS)
        end_window("editing the pattern and filling the menus with the USDVariant panel open")

        check(options(variant, "variantSets") == [UNSEARCHED_ID], "the Variant Sets menu holds only the unsearched entry (got %r)" % (options(variant, "variantSets"),))
        def refuses_unsearched(node, knob):
            box, items = combo_texts(node, knob)
            check(box is not None and any(UNSEARCHED_TEXT in i for i in items),
                  "the %s combo shows an 'N unloaded branches not searched' entry (items %r)" % (knob, items))
            if box is not None:
                index = [n for n, i in enumerate(items) if UNSEARCHED_TEXT in i][0]
                before = node.getParam(knob).getValue()
                box.setCurrentIndex(index)
                QApplication.processEvents()
                check(node.getParam(knob).getValue() == before, "the %s combo refuses the unsearched entry (disabled)" % knob)

        for knob in ("variantSets", "variants"):
            refuses_unsearched(variant, knob)
        camera_options = options(settings, "camera")
        check(CAMERA in camera_options and UNSEARCHED_ID in camera_options,
              "the camera menu lists %s and the unsearched entry (got %r)" % (CAMERA, camera_options))
        opened = yield from open_panel(settings)
        check(opened, "the USDRenderSettings panel opens again")
        yield ("sleep", SETTLE_MS)
        refuses_unsearched(settings, "camera")
        opened = yield from open_panel(variant)
        check(opened, "the USDVariant panel opens again")
        yield ("sleep", SETTLE_MS)
        check(opened_buildings() == [], "filling the menus opens no building's layer (got %d)" % len(opened_buildings()))
        check(load_edits() == 0, "filling the menus loads nothing in the viewer (load edits %r)" % (load_edits(),))
        shot("m381-menus-unloaded.png")

        # 2. One building loaded in the viewer from the Scene Graph panel. The window starts before
        # the twirl that commits the load, so that the refill it sends lands inside the window.
        section("menus after a load")
        home = view()
        while home is not None and home.metaObject().className().split("::")[-1] != "TabWidget":
            home = home.parentWidget()
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        ok = yield ("until", lambda: panel() is not None and panel().isVisible() and find(WORLD).isValid(), POLL_TIMEOUT_S)
        require(ok, "the Scene Graph panel lists /World")
        for path in (WORLD, BLOCK0):
            tree().expand(find(path))
        ok = yield ("until", lambda: state(TARGET) == STATE_UNLOADED, POLL_TIMEOUT_S)
        require(ok, "the target building's row is listed as unloaded (state %r)" % (state(TARGET),))
        target_row = find(TARGET)
        target_tree = tree()
        target_view = view()
        require(target_row.isValid() and target_tree is not None and target_view is not None, "the target's row and the scene view are found")
        edits = load_edits()
        require(VARIANT_SET not in options(variant, "variantSets"),
                "the Variant Sets menu has not refilled before the load (got %r)" % (options(variant, "variantSets"),))
        window.start()
        target_tree.expand(target_row)
        ok = yield ("until", lambda: target_view.property("natronSceneLoadEdits") != edits, POLL_TIMEOUT_S)
        check(ok, "the load moves the viewer's load edits (%r, then %r)" % (edits, load_edits()))
        ok = yield ("until", lambda: VARIANT_SET in options(variant, "variantSets"), POLL_TIMEOUT_S)
        check(ok, "the Variant Sets menu refills with %r (got %r)" % (VARIANT_SET, options(variant, "variantSets")))
        ok = yield ("until", lambda: set(VARIANTS) <= set(options(variant, "variants")), POLL_TIMEOUT_S)
        check(ok, "the Variants menu lists %r (got %r)" % (VARIANTS, options(variant, "variants")))
        yield ("sleep", SETTLE_MS)
        end_window("refilling the menus after the load")
        ok = yield ("until", lambda: state(TARGET) == STATE_LOADED, POLL_TIMEOUT_S)
        check(ok, "the twirled building loads (state %r)" % (state(TARGET),))
        check(len(opened_buildings()) == 1, "one building layer is open (got %d)" % len(opened_buildings()))
        report("NOTE after the load: sets %r, variants %r" % (options(variant, "variantSets"), options(variant, "variants")))
        ok = yield ("until", lambda: loads_quiet() and scene_drawn(), POLL_TIMEOUT_S)
        check(ok, "the viewer settles after the load")

        # 3. Centre Pivot.
        section("centre pivot")
        view_node = app.getViewer(viewer.getScriptName())
        require(view_node is not None, "the Viewer has a Python wrapper")
        view_node.seek(FRAME_A)
        yield ("sleep", SETTLE_MS)
        initial = pivot()
        check(initial == (0.0, 0.0, 0.0), "the pivot starts at the origin (got %r)" % (initial,))
        window.start()
        edit.getParam("centrePivot").trigger()
        ok = yield ("until", lambda: pivot() != initial, POLL_TIMEOUT_S)
        check(ok, "Centre Pivot changes the pivot (got %r)" % (pivot(),))
        yield ("sleep", SETTLE_MS)
        end_window("Centre Pivot")
        first = pivot()
        report("NOTE Centre Pivot at frame %d gave %r" % (FRAME_A, first))
        check(abs(first[0] - START_X) < 0.5, "the pivot x is the building's centre at frame %d, %g (got %r)" % (FRAME_A, START_X, first))

        edit.getParam("pivot").set(0.0, 0.0, 0.0)
        yield ("sleep", SETTLE_MS)
        check(pivot() == (0.0, 0.0, 0.0), "the pivot is reset for the double press (got %r)" % (pivot(),))
        view_node.seek(FRAME_A)
        window.start()
        edit.getParam("centrePivot").trigger()
        view_node.seek(FRAME_B)
        edit.getParam("centrePivot").trigger()
        ok = yield ("until", lambda: pivot() != (0.0, 0.0, 0.0), POLL_TIMEOUT_S)
        check(ok, "two quick presses change the pivot (got %r)" % (pivot(),))
        yield ("sleep", SETTLE_MS * 2)
        end_window("two quick Centre Pivot presses")
        second = pivot()
        report("NOTE the double press gave %r" % (second,))
        check(abs(second[0] - END_X) < 0.5,
              "the pivot comes from the second press (frame %d, x %g; got %r)" % (FRAME_B, END_X, second))
        check(pivot() == second, "the pivot stays put once settled: no late write from the first job")
        check(len(opened_buildings()) == 1, "Centre Pivot loads no other building (got %d layers)" % len(opened_buildings()))
        shot("m381-centre-pivot.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
