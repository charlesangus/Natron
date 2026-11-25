"""A geometry loop in the GUI: GeoPrimitive -> GeoForEachBegin -> GeoTransform -> GeoForEachEnd ->
GeoToUSD -> Viewer1.

Graph: a GeoPrimitive cube with one face per side. The GeoForEachBegin extracts pieces. The GeoTransform's translate x is
driven by loop.iteration, from 0 to PIECE_SPACING. The GeoForEachEnd iterates over pieces with Piece
Method Each Element, so each of the cube's 6 faces is its own piece. The stage is read by flattening
the GeoToUSD to a .usda through a probe USDWrite and opening it with pxr.

Sections:
  - pieces: the stage holds every piece, 6 faces of 4 points each, spread along x by the driver,
    and the scene view shows it with no error;
  - cancel: with a heartbeat running (a 5 ms Qt timer on the main thread, recording the wall-clock
    gap between ticks), a GeoWrangle running HEAVY_STEPS steps per point is put between the
    GeoTransform and the End, and the End is switched to Iteration Method Count with LONG_ITERATIONS
    iterations and Max Iterations set to match. After LONG_COOK_MS the End and the GeoWrangle must
    report no error and no scene may have synced, so the long loop is still cooking. Iterations is
    then set to SHORT_ITERATIONS. The scene view must
    show the short loop within CANCEL_TIMEOUT_S, and the longest heartbeat gap must stay under
    MAX_GAP_MS. After the heartbeat stops, the stage must hold SHORT_ITERATIONS cubes.
Screenshots: m402_geo_foreach_pieces.png and m402_geo_foreach_cancelled.png in $NATRON_GUI_TEST_OUT.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m402_geo_foreach.py
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 700
CANCEL_TIMEOUT_S = float(os.environ.get("NATRON_TEST_CANCEL_TIMEOUT_S", "10"))
MAX_GAP_MS = float(os.environ.get("NATRON_TEST_MAX_GAP_MS", "100"))
LONG_COOK_MS = 3000
LONG_SETTLE_MS = 750
PIECE_SPACING = 3.0
CUBE_FACES = 6
CUBE_POINTS = 8
LONG_ITERATIONS = 100000
HEAVY_STEPS = 2000000
SHORT_ITERATIONS = 3
# GeoPrimitive's Type, GeoForEachBegin's Method, GeoForEachEnd's choices.
SHAPE_CUBE = 2
METHOD_EXTRACT = 0
ITERATE_PIECES = 0
ITERATE_COUNT = 1
PIECES_EACH_ELEMENT = 2

try:
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication
    from pxr import Usd, UsdGeom

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, widgets_of

    work_dir = os.path.join(OUT, "work")
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a %s parameter" % (node.getScriptName(), name))
        return p

    cube = make("GeoPrimitive", 0, 0)
    param(cube, "type").setValue(SHAPE_CUBE)
    param(cube, "cubeSubdivisions").set(1, 1, 1)  # one face per side, as CUBE_FACES and CUBE_POINTS count
    begin = make("GeoForEachBegin", 0, 100)
    require(begin.connectInput(0, cube), "connect the GeoPrimitive into the GeoForEachBegin")
    param(begin, "method").setValue(METHOD_EXTRACT)
    move = make("GeoTransform", 0, 200)
    require(move.connectInput(0, begin), "connect the GeoForEachBegin into the GeoTransform")
    require(param(move, "translate").setDriver("loop.iteration", 0.0, PIECE_SPACING, "linear", 0),
            "drive the GeoTransform's translate x from loop.iteration")
    end = make("GeoForEachEnd", 0, 300)
    require(end.connectInput(0, move), "connect the GeoTransform into the GeoForEachEnd")
    param(end, "iterationMethod").setValue(ITERATE_PIECES)
    param(end, "pieceMethod").setValue(PIECES_EACH_ELEMENT)
    to_usd = make("GeoToUSD", 0, 400)
    require(to_usd.connectInput(0, end), "connect the GeoForEachEnd into the GeoToUSD")

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 500)

    # Kept for the whole run: the child wrappers die with their parent wrapper.
    probe = make("USDWrite", 300, 400)
    counter = [0]

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def synced_at_ms():
        v = view()
        return (v.property("natronSceneSyncedAtMs") or 0) if v is not None else 0

    def now_ms():
        return int(time.time() * 1000)

    def read_stage():
        """Flattens the GeoToUSD to a fresh .usda and opens it with pxr. Blocks the main thread."""
        counter[0] += 1
        path = os.path.join(work_dir, "foreach%d.usda" % counter[0])
        probe.disconnectInput(0)
        require(probe.connectInput(0, to_usd), "connect the probe writer to the GeoToUSD")
        probe.getParam("filename").setValue(path)
        probe.getParam("exportMode").setValue(1)  # flattened
        if os.path.exists(path):
            os.remove(path)
        app.renderBlocking(probe, 1, 1)
        return Usd.Stage.Open(path) if os.path.isfile(path) else None

    def meshes(stage):
        """(faces, points, min x, max x) over every Mesh of the stage."""
        faces = points = 0
        lo = hi = None
        for prim in stage.Traverse():
            mesh = UsdGeom.Mesh(prim)
            if not mesh:
                continue
            faces += len(mesh.GetFaceVertexCountsAttr().Get() or [])
            for p in mesh.GetPointsAttr().Get() or []:
                points += 1
                lo = p[0] if lo is None else min(lo, p[0])
                hi = p[0] if hi is None else max(hi, p[0])
        return faces, points, lo, hi

    gaps = {"last": None, "max_ms": 0.0, "ticks": 0}
    heartbeat = QTimer()
    heartbeat.setInterval(5)

    def tick():
        now = time.monotonic()
        if gaps["last"] is not None:
            gaps["max_ms"] = max(gaps["max_ms"], (now - gaps["last"]) * 1000.0)
        gaps["last"] = now
        gaps["ticks"] += 1

    heartbeat.timeout.connect(tick)

    def steps():
        # pieces
        stage = read_stage()
        require(stage is not None, "the GeoToUSD flattens to a stage")
        faces, points, lo, hi = meshes(stage)
        check(faces == CUBE_FACES, "every piece is in the stage: %d faces (%d)" % (CUBE_FACES, faces))
        check(points == CUBE_FACES * 4, "each face piece keeps its own 4 points: %d points (%d)" % (CUBE_FACES * 4, points))
        spread = (hi - lo) if (lo is not None and hi is not None) else 0.0
        check(spread >= PIECE_SPACING * (CUBE_FACES - 1),
              "the pieces are spread along x by the driver (%r)" % spread)

        require(viewer.connectInput(0, to_usd), "connect the Viewer to the GeoToUSD")
        ok = yield ("until", lambda: view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the scene view is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        check(ok, "the scene view shows the pieces (%r)" % (v.property("natronSceneError") if v else None,))
        yield ("sleep", SETTLE_MS)
        shot("m402_geo_foreach_pieces.png")

        # cancel
        heartbeat.start()
        param(end, "maxIterations").setValue(LONG_ITERATIONS)
        param(end, "iterations").setValue(LONG_ITERATIONS)
        heavy = make("GeoWrangle", 150, 250)
        param(heavy, "snippet").setValue("for (int k = 0; k < %d; k++) f@heavy += k * 0.5;\n" % HEAVY_STEPS)
        require(heavy.connectInput(0, move), "connect the GeoTransform into the GeoWrangle")
        end.disconnectInput(0)
        require(end.connectInput(0, heavy), "connect the GeoWrangle into the GeoForEachEnd")
        param(end, "iterationMethod").setValue(ITERATE_COUNT)
        # Each edit above can sync a quick intermediate scene (the pieces loop, or the old iteration count)
        # before the long loop starts cooking, so the baseline is taken once those have landed.
        yield ("sleep", LONG_SETTLE_MS)
        baseline_ms = synced_at_ms()
        yield ("sleep", LONG_COOK_MS)
        # scene_drawn() stays true on the previous scene while a new one cooks, so it cannot show a cook running.
        check(synced_at_ms() == baseline_ms,
              "the long loop is still cooking after %d ms (no scene synced since its edit)" % LONG_COOK_MS)
        for node in (end, heavy):
            message = str(node.getPersistentMessage() or "")
            require(not message, "%s reports no error while the long loop cooks (%r)" % (node.getScriptName(), message))
        started = time.monotonic()
        short_edit_ms = now_ms()
        param(end, "iterations").setValue(SHORT_ITERATIONS)
        ok = yield ("until", lambda: synced_at_ms() >= short_edit_ms and scene_drawn(), CANCEL_TIMEOUT_S)
        took = time.monotonic() - started
        heartbeat.stop()
        report("NOTE short loop shown %.2f s after the edit; longest heartbeat gap %.1f ms over %d ticks"
               % (took, gaps["max_ms"], gaps["ticks"]))
        check(gaps["max_ms"] < MAX_GAP_MS,
              "the GUI thread never stalls for %g ms or more (%.1f ms)" % (MAX_GAP_MS, gaps["max_ms"]))
        # The probe below renders on the main thread with no timeout, behind a long loop that may still be cooking.
        require(ok, "the long loop is cancelled and the short one shown within %g s" % CANCEL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        shot("m402_geo_foreach_cancelled.png")

        report("NOTE iterations reads %d before the probe" % param(end, "iterations").getValue())
        stage = read_stage()
        require(stage is not None, "the GeoToUSD flattens to a stage after the cancel")
        faces, points, _, _ = meshes(stage)
        check(faces == SHORT_ITERATIONS * CUBE_FACES and points == SHORT_ITERATIONS * CUBE_POINTS,
              "the stage holds %d cubes (%d faces, %d points)" % (SHORT_ITERATIONS, faces, points))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
