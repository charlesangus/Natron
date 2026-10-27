"""Edit-to-frame latency of the scene viewer.

Two modes:

  python3 tools/bench/scene_edit_bench.py make-file OUT.usdc [--prims N]
      Writes a flat test file of N cubes (default 1000000) under /World, with the container's
      pxr Python module (the natron-dev image has it). Run once per size.

  Tests/gui/run-gui-test.sh tools/bench/scene_edit_bench.py [natron-binary]
      Runs inside Natron (GUI, under Xvfb) and is configured by the environment:
        BENCH_SCENE   chain (default) or file
        BENCH_FILE    the USD file USDRead reads (file mode; default Tests/fixtures/scene-reference.usda)
        BENCH_N       chain length (chain mode, default 30)
        BENCH_OP      plug-in ID of the scene op chained (default USDEditTransform; falls back to Dot,
                      which authors nothing, when the build has no such plug-in)
        BENCH_KNOB    the knob edited: on the chain's head op (chain mode, default translate),
                      or on USDRead (file mode, default frameOffset)
        BENCH_EDITS   edits timed (default 20), after 3 untimed warm-up edits
        BENCH_LABEL   free text stored with the result (for example "before" or "after")
        BENCH_RESULT  JSON-lines file the result is appended to
                      (default $NATRON_GUI_TEST_OUT/scene_edit_bench.jsonl)

      Each edit sets the knob, then waits for the scene view to have synced the edit and drawn a
      finished frame after that. Latency is the edit to that frame, from the view's
      natronSceneSyncedAtMs and natronSceneLastPaintMs properties; it also reports the open time
      (connect to first frame) and whether the view kept one engine (natronSceneEngineCount).
      Builds without those properties (before the long-lived scene) give only the open time and
      poll-rounded latencies (marked "approximate").
"""
import json
import os
import sys
import time
import traceback


def make_file(argv):
    from pxr import Sdf, Usd, UsdGeom  # noqa: F401 (UsdGeom registers the schema)

    out = argv[0]
    prims = 1000000
    if "--prims" in argv:
        prims = int(argv[argv.index("--prims") + 1])
    layer = Sdf.Layer.CreateNew(out)
    with Sdf.ChangeBlock():
        layer.defaultPrim = "World"
        layer.pseudoRoot.SetInfo("upAxis", "Y")
        world = Sdf.CreatePrimInLayer(layer, "/World")
        world.specifier = Sdf.SpecifierDef
        world.typeName = "Xform"
        side = max(1, int(round(prims ** (1.0 / 3.0))))
        for i in range(prims):
            prim = Sdf.PrimSpec(world, "c%d" % i, Sdf.SpecifierDef, "Cube")
            size = Sdf.AttributeSpec(prim, "size", Sdf.ValueTypeNames.Double)
            size.default = 0.5
            translate = Sdf.AttributeSpec(prim, "xformOp:translate", Sdf.ValueTypeNames.Double3)
            translate.default = (float(i % side), float((i // side) % side), float(i // (side * side)))
            order = Sdf.AttributeSpec(prim, "xformOpOrder", Sdf.ValueTypeNames.TokenArray, variability=Sdf.VariabilityUniform)
            order.default = ["xformOp:translate"]
    layer.Save()
    print("wrote %s with %d prims" % (out, prims))


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "make-file":
    make_file(sys.argv[2:])
    sys.exit(0)

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

TIMEOUT_S = float(os.environ.get("BENCH_TIMEOUT_S", "600"))
WARMUP = 3

try:
    from guitest import FIXTURES, OUT, check, finish, report, require, run, widgets_of

    scene = os.environ.get("BENCH_SCENE", "chain")
    n = int(os.environ.get("BENCH_N", "30"))
    edits = int(os.environ.get("BENCH_EDITS", "20"))
    path = os.environ.get("BENCH_FILE", os.path.join(FIXTURES, "scene-reference.usda"))
    result_path = os.environ.get("BENCH_RESULT", os.path.join(OUT, "scene_edit_bench.jsonl"))
    require(os.path.isfile(path), "the file exists: " + path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead")
    usd.getParam("filename").setValue(path)
    head = usd
    edited = usd
    knob_name = os.environ.get("BENCH_KNOB", "frameOffset")
    op_id = None
    if scene == "chain":
        op_id = os.environ.get("BENCH_OP", "USDEditTransform")
        previous = usd
        for i in range(n):
            op = app.createNode(op_id)
            if op is None:
                op_id = "fr.inria.built-in.Dot"
                op = app.createNode(op_id)
            require(op is not None, "create chain node %d" % i)
            op.connectInput(0, previous)
            if i == 0:
                head = op
            previous = op
        knob_name = os.environ.get("BENCH_KNOB", "translate" if op_id != "fr.inria.built-in.Dot" else "frameOffset")
        edited = head if head.getParam(knob_name) is not None else usd
        tail = previous
    else:
        tail = usd
    knob = edited.getParam(knob_name)
    require(knob is not None, "%s has a %s parameter" % (edited.getScriptName(), knob_name))
    viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def prop(name, default=None):
        v = view()
        value = v.property(name) if v is not None else None
        return default if value is None else value

    precise = None

    def shown():
        return bool(prop("natronSceneShown", False)) and not prop("natronSceneError")

    def set_knob(i):
        value = float(i + 1)
        try:
            knob.setValue(value)
        except TypeError:
            knob.setValue(value, 0)

    def steps():
        global precise
        started = time.time()
        require(viewer.connectInput(0, tail), "connect the Viewer")
        ok = yield ("until", lambda: shown() and prop("natronSceneLastPaintMs") is not None or (shown() and prop("natronSceneSynced") is None), TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        open_ms = (time.time() - started) * 1000.0
        precise = prop("natronSceneSyncedAtMs") is not None
        engines_before = prop("natronSceneEngineCount", -1)
        latencies = []
        for i in range(WARMUP + edits):
            t0 = time.time() * 1000.0
            set_knob(i)

            def drawn():
                if not precise:
                    return shown()
                synced_at = prop("natronSceneSyncedAtMs", 0)
                painted_at = prop("natronSceneLastPaintMs", 0)
                return bool(prop("natronSceneSynced", False)) and synced_at >= t0 and painted_at >= synced_at

            ok = yield ("until", drawn, TIMEOUT_S)
            check(ok, "edit %d reached the screen" % i)
            if not precise:
                yield ("sleep", 0)
            latency = (prop("natronSceneLastPaintMs", 0) - t0) if precise else (time.time() * 1000.0 - t0)
            if i >= WARMUP:
                latencies.append(latency)
        latencies.sort()
        record = {
            "label": os.environ.get("BENCH_LABEL", ""),
            "scene": scene,
            "file": os.path.basename(path),
            "n": n if scene == "chain" else 0,
            "op": op_id,
            "knob": knob_name,
            "edits": edits,
            "open_ms": round(open_ms, 1),
            "edit_ms_median": round(latencies[len(latencies) // 2], 1) if latencies else None,
            "edit_ms_p90": round(latencies[int(len(latencies) * 0.9) - 1], 1) if latencies else None,
            "edit_ms_max": round(latencies[-1], 1) if latencies else None,
            "approximate": not precise,
            "engines_before": engines_before,
            "engines_after": prop("natronSceneEngineCount", -1),
            "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
        }
        with open(result_path, "a") as f:
            f.write(json.dumps(record) + "\n")
        report("RESULT " + json.dumps(record))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
