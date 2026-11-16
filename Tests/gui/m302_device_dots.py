"""GUI check that a node's halo shows the device it ran on and stays off when the cache served it.

Graph, two branches into one Viewer:
  CPU branch: CheckerBoard (Force Caching on) -> Grade. Their region weighs less than the GPU
              placement threshold, so they render on the CPU.
  GPU branch: CheckerBoard -> Blur. The Blur's filter weight sends the region to the GPU.
The Viewer shows each branch in turn while one parameter of that branch is stepped every
BUMP_MS for SWEEP_S, and the halo either side of each node is sampled from screen grabs
throughout (it is lit for one refresh interval after each execution).

CPU half: the Grade's halo is yellow on both sides and never the GPU colour; the cached
CheckerBoard above it stays unlit because the cache served it.
GPU half: the Blur's halo is the GPU colour on both sides. It runs only when natron.log says
GPU compute is ready on a device: under NATRON_GPU=force any Vulkan device does (lavapipe counts),
under Auto only a hardware one. Otherwise it is skipped with a SKIP line in results.txt. A halo split
yellow/GPU (left/right), when a node ran on both devices within one refresh, is reported as
a NOTE: this graph has no node that does so deterministically.

Run through Tests/gui/run-gui-test.sh at 1920x1080 (add -e NATRON_GPU=force and a Vulkan ICD
for the GPU half). Screenshots (m302-device-dots-*.png) and results.txt go to
$NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 3000
SWEEP_S = 6.0
BUMP_MS = 700
WATCHDOG_S = 280
ZOOM = 2.0
# Scene distances out from a node's box at which the halo is sampled; the halo's inner edge moves
# out by the user colour border's width when that border is shown.
HALO_OFFSETS = (1.5, 3.0, 4.5, 6.0, 7.5, 9.0)
GPU_READY = "GPU compute ready on "
GPU_UNAVAILABLE = "GPU compute is unavailable: "

try:
    import NatronEngine
    from PySide6.QtCore import QPointF, QRect
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    PROPS = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}

    def is_cpu_colour(c):
        return c[0] > 200 and c[1] > 200 and c[2] < 80

    def is_gpu_colour(c):
        return c[0] < 130 and 110 < c[1] < 195 and c[2] > 210

    def create(plugin_id, x, y):
        node = app.createNode(plugin_id, -1, None, PROPS)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def set_any(param, value):
        """Sets every dimension of param to value, whatever its dimension count."""
        for n in (4, 3, 2, 1):
            try:
                param.set(*([value] * n))
                return True
            except TypeError:
                continue
        return False

    def gpu_log_line():
        """The line of natron.log saying whether GPU compute came up, or None before it is written.
        The device menu cannot tell: its options are device UUIDs, not names."""
        path = os.path.join(gt.OUT, "natron.log")
        if not os.path.isfile(path):
            return None
        with open(path, errors="replace") as f:
            for line in f:
                if GPU_READY in line or GPU_UNAVAILABLE in line:
                    return line.strip()
        return None

    def gpu_expected():
        """(whether GPU compute can use a device, why), from NATRON_GPU, the compute preference and
        what natron.log says GPU compute did at startup."""
        if os.environ.get("NATRON_GPU", "").strip().lower() == "off":
            return False, "NATRON_GPU=off"
        settings = NatronEngine.natron.getSettings()
        mode = settings.getParam("gpuComputeMode")
        mode_label = mode.getOption(mode.get()).lower() if mode is not None else ""
        if "off" in mode_label:
            return False, "the GPU compute preference is %r" % mode_label
        line = gpu_log_line()
        if line is None:
            return False, "natron.log says neither %r nor %r" % (GPU_READY.strip(), GPU_UNAVAILABLE.strip())
        return GPU_READY in line, line

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def frame_nodes(nodes):
        view = graph_view()
        if view is None:
            return False
        xs, ys = [], []
        for n in nodes:
            x, y = n.getPosition()[0:2]
            w, h = n.getSize()[0:2]
            xs += [x, x + w]
            ys += [y, y + h]
        view.resetTransform()
        view.scale(ZOOM, ZOOM)
        view.centerOn((min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0)
        QApplication.processEvents()
        return True

    def halo_points(view, node):
        """Global screen points left and right of node's box, out through the halo."""
        x, y = node.getPosition()[0:2]
        w, h = node.getSize()[0:2]
        mid = y + h / 2.0
        to_screen = lambda sx, sy: view.viewport().mapToGlobal(view.mapFromScene(QPointF(sx, sy)))
        return ([to_screen(x - d, mid) for d in HALO_OFFSETS],
                [to_screen(x + w + d, mid) for d in HALO_OFFSETS])

    def side_state(image, origin, points):
        colours = []
        for p in points:
            px, py = p.x() - origin[0], p.y() - origin[1]
            if 0 <= px < image.width() and 0 <= py < image.height():
                c = image.pixelColor(px, py)
                colours.append((c.red(), c.green(), c.blue()))
        if any(is_cpu_colour(c) for c in colours):
            return "cpu"
        if any(is_gpu_colour(c) for c in colours):
            return "gpu"
        return None

    def halo_states(nodes):
        """{script name: (left, right)} with each side "cpu", "gpu" or None, from one grab of the
        region around nodes; None when the node graph is not visible. A screen grab, since a
        widget grab can come out blank under llvmpipe."""
        view = graph_view()
        if view is None:
            return None
        sides = {n.getScriptName(): halo_points(view, n) for n in nodes}
        every = [p for left, right in sides.values() for p in left + right]
        x0 = min(p.x() for p in every) - 2
        y0 = min(p.y() for p in every) - 2
        rect = QRect(x0, y0, max(p.x() for p in every) - x0 + 3, max(p.y() for p in every) - y0 + 3)
        image = QGuiApplication.primaryScreen().grabWindow(0, rect.x(), rect.y(), rect.width(), rect.height()).toImage()
        origin = (rect.x(), rect.y())
        return {name: (side_state(image, origin, left), side_state(image, origin, right))
                for name, (left, right) in sides.items()}

    def sweep(nodes, bump, shot_when, shot_name):
        """Steps the branch with bump for SWEEP_S while sampling the halos of nodes. Returns
        {script name: set of (left, right) states seen}; saves shot_name the first time
        shot_when(states) holds, while the halo is still lit."""
        seen = {n.getScriptName(): set() for n in nodes}
        state = {"start": time.time(), "bumped": 0.0, "n": 0, "shot": None, "error": None}

        def poll():
            now = time.time()
            if now - state["bumped"] >= BUMP_MS / 1000.0:
                state["n"] += 1
                bump(state["n"])
                state["bumped"] = now
            try:
                states = halo_states(nodes)
            except Exception:
                if state["error"] is None:
                    state["error"] = traceback.format_exc()
                states = None
            if states:
                for name, s in states.items():
                    seen[name].add(s)
                if state["shot"] is None and shot_when(states):
                    state["shot"] = states
                    gt.shot(shot_name)
            return now - state["start"] >= SWEEP_S

        return seen, state, poll

    def lit_both(seen, name, device):
        return (device, device) in seen[name]

    def ever(seen, name, device):
        return any(device in s for s in seen[name])

    def steps():
        ok = yield ("until", lambda: bool(gt.widgets_of("NodeGraph")), STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        gt.close_all_panels()

        gt.section("setup")
        cpu_src = create("CheckerBoard", 0, 0)
        cached = cpu_src.getParam("forceCaching")
        require(cached is not None, "the CheckerBoard has a forceCaching parameter")
        cached.set(True)
        grade = create("Grade", 0, 150)
        require(grade.connectInput(0, cpu_src), "the CPU CheckerBoard feeds the Grade")
        multiply = grade.getParam("multiply")
        require(multiply is not None, "the Grade has a multiply parameter")
        gpu_src = create("CheckerBoard", 300, 0)
        blur = create("Blur", 300, 150)
        require(blur.connectInput(0, gpu_src), "the GPU CheckerBoard feeds the Blur")
        size = blur.getParam("size")
        require(size is not None, "the Blur has a size parameter")
        require(set_any(size, 8.0), "the Blur size can be set")
        viewer = app.getNode("Viewer1") or create("Viewer", 150, 350)
        require(viewer is not None, "a Viewer exists")
        app.clearSelection()
        gt.close_all_panels()
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is visible")
        all_nodes = [cpu_src, grade, gpu_src, blur]
        require(frame_nodes(all_nodes), "the node graph is visible")
        yield ("until", lambda: gpu_log_line() is not None, STARTUP_TIMEOUT_S)
        gpu_on, why = gpu_expected()
        report("NOTE GPU half %s: %s" % ("runs" if gpu_on else "skipped", why))

        gt.section("cpu")
        require(viewer.connectInput(0, grade), "the Viewer shows the Grade")
        yield ("sleep", SETTLE_MS)
        app.clearSelection()
        cpu_nodes = [cpu_src, grade]
        g, c = grade.getScriptName(), cpu_src.getScriptName()
        seen, state, poll = sweep(cpu_nodes, lambda n: set_any(multiply, 1.0 + 0.05 * (n % 8)),
                                  lambda s: s[g] == ("cpu", "cpu") and s[c] == (None, None),
                                  "m302-device-dots-cpu.png")
        yield ("until", poll, SWEEP_S + 30.0)
        check(state["error"] is None, "the halos could be sampled%s" % (": " + state["error"] if state["error"] else ""))
        report("NOTE CPU sweep: %d steps; Grade halo states %r; cached CheckerBoard %r" %
               (state["n"], sorted(seen[g], key=str), sorted(seen[c], key=str)))
        check(lit_both(seen, g, "cpu"), "the Grade's halo turns yellow on both sides after it runs on the CPU")
        check(not ever(seen, g, "gpu"), "the Grade's halo never shows the GPU colour")
        check(not ever(seen, c, "cpu") and not ever(seen, c, "gpu"),
              "the cached CheckerBoard's halo stays unlit while the Grade below it re-runs")
        check(state["shot"] is not None, "one grab shows the Grade lit and the cached CheckerBoard unlit together")
        gt.shot("m302-device-dots-cpu-after.png")

        gt.section("gpu")
        if not gpu_on:
            report("SKIP GPU half: %s. The CPU dot and the cache-hit check above still ran." % why)
            faulthandler.cancel_dump_traceback_later()
            return
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, blur), "the Viewer shows the Blur")
        yield ("sleep", SETTLE_MS)
        app.clearSelection()
        b = blur.getScriptName()
        seen, state, poll = sweep([gpu_src, blur], lambda n: set_any(size, 8.0 + (n % 6)),
                                  lambda s: s[b] == ("gpu", "gpu"), "m302-device-dots-gpu.png")
        yield ("until", poll, SWEEP_S + 30.0)
        check(state["error"] is None, "the halos could be sampled%s" % (": " + state["error"] if state["error"] else ""))
        report("NOTE GPU sweep: %d steps; Blur halo states %r; its CheckerBoard %r" %
               (state["n"], sorted(seen[b], key=str), sorted(seen[gpu_src.getScriptName()], key=str)))
        check(lit_both(seen, b, "gpu"),
              "the Blur's halo shows the GPU colour on both sides after it runs on the device "
              "(if only yellow was seen, placement kept the region on the CPU)")
        if ("cpu", "gpu") in seen[b]:
            report("NOTE the Blur's halo was split yellow/GPU in one grab: it ran on both devices within a refresh")
        gt.shot("m302-device-dots-gpu-after.png")
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
