"""SplineWarp follows a source curve and a correspondence point through motion vectors, in the app.

A ctest drives the follow without a viewer; this checks what a user sees. Plate, scene and follow
helpers are in m219_follow_common.py.

A source square is linked to a destination square, and a correspondence is added halfway along the
first edge of both. The source curve is selected in the curve list and the correspondence by a click on
its source end with the correspondence tool, then the viewer row's Forward To End follows both through a
VectorGenerator over the panning plate:

  row          the viewer shows SplineWarp's Follow Vectors row, every control on screen
  follow       the follow is done and its report lists the curve and the correspondence
  source       every point of the source curve is within POS_TOL of the known pan on every frame
  destination  the destination curve keeps its one key and its points
  correspondence  the correspondence's destination end slides along the still destination curve
  link         the curves stay linked to each other

The warped output is scrubbed and a screenshot taken at the first, a middle and the last frame
(m220-splinewarp-follow-<frame>.png in the output directory).

Usage (natron-dev image; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker run --rm --memory 12g -v $PWD:$PWD -w $PWD \\
        -e NATRON_GUI_TEST_OUT=build/m220-splinewarp-follow-vectors/out -e GUI_TEST_TIMEOUT=900 \\
        ghcr.io/charlesangus/natron-dev:<hash> \\
        Tests/gui/run-gui-test.sh Tests/gui/m220_splinewarp_follow.py <Natron AppImage>
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 3000
DEST_OFFSET = (-50.0, -50.0)
CORRESPONDENCE_PARAM = 0.5
# The pan moves the picture a few pixels along any edge over the range; a param is in edges.
SLID_MIN = 0.05

try:
    import faulthandler

    from PySide6.QtWidgets import QApplication

    import guitest as gt
    import roto_gui as rg
    from guitest import check, report, require
    from m219_follow_common import *
    import m219_follow_common

    m219_follow_common.bind(app)

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def correspondence_at(link, t):
        """(source param, destination param) of correspondence 0 at t."""
        params = list(link.getCorrespondences(float(t)))
        flat = []
        for p in params:
            if isinstance(p, (tuple, list)):
                flat.extend(p)
            else:
                flat.append(p)
        require(len(flat) >= 2, "read correspondence 0 at frame %d (%r)" % (t, params))
        return float(flat[0]), float(flat[1])

    def midpoint(bez, a, b, t):
        pa, pb = pos(bez, a, t), pos(bez, b, t)
        return ((pa[0] + pb[0]) / 2.0, (pa[1] + pb[1]) / 2.0)

    def open_scene_warp(scene):
        scene.roto, scene.ctx = rg.open_roto(app, "SplineWarp")
        require(scene.roto is not None and scene.ctx is not None, "create a SplineWarp with its panel open", gt.plugin_problem("SplineWarp"))
        keep(scene.roto)
        keep(scene.ctx)
        scene.sw = keep(scene.roto.getSplineWarp())
        require(scene.sw is not None, "the SplineWarp has links")
        require(scene.roto.connectInput(0, scene.read), "connect SplineWarp to the plate")
        rg.activate()
        yield from settle()

    def section_follow(scene):
        gt.section("row")
        count = yield from check_row("SplineWarp")
        report("NOTE SplineWarp: %d of %d row controls found" % (count, len(HINTS)))

        gt.section("follow")
        set_project_range(START, MAIN_LAST)
        ensure_source(scene.roto, scene.gen)
        set_choice(scene.roto, "followModel", MODEL_IDS["Per Point"])
        source = new_shape(scene.ctx)
        destination = new_shape(scene.ctx, SQ_X + DEST_OFFSET[0], SQ_Y + DEST_OFFSET[1])
        link = scene.sw.link(source, destination)
        require(link is not None, "link the source square to the destination square")
        keep(link)
        require(link.addCorrespondence(CORRESPONDENCE_PARAM, CORRESPONDENCE_PARAM) == 0, "add a correspondence halfway along the first edges")
        yield from go_to(START)
        source_start = [pos(source, i, START) for i in range(POINTS)]
        destination_start = [pos(destination, i, START) for i in range(POINTS)]
        corr_start = correspondence_at(link, START)

        select_shapes([source])
        gl = rg.viewer_gl()
        require(gl is not None, "the viewer is visible")
        keep(gl)
        rg.activate()
        yield ("sleep", SETTLE_MS)
        vmap = rg.ViewerMap(gl)
        require(vmap.ok, "calibrated the viewer")
        require(rg.pick_tool("CorrespondenceTools", "CorrespondenceTool"), "pick the correspondence tool")
        yield ("sleep", SETTLE_MS)
        x, y = midpoint(source, 0, 1, START)
        rg.click(gl, vmap.to_widget(x, y))
        yield ("sleep", SEEK_MS)
        check(link.getCorrespondenceCount() == 1, "the click selected the correspondence and added none (%d)" % link.getCorrespondenceCount())

        outcome = yield from run_follow(scene, "followForwardToEnd", "SplineWarp, source curve and correspondence")
        names = sorted(outcome["shapes"].keys())
        check(len(names) == 2 and source.getScriptName() in names and any(n.startswith("correspondence") for n in names),
              "follow: the report lists the source curve and the correspondence (%r)" % names)

        frames = list(range(START, MAIN_LAST + 1))
        worst = max_deviation(source, frames, lambda t, i: shifted(source_start[i], *known_shift(t)))
        check(float(MAIN_LAST) in keys_of(source) and worst <= POS_TOL,
              "source: the curve is keyed to the last frame and within %.2f px of the pan (keys %r, largest error %.4f)" % (POS_TOL, keys_of(source), worst))
        still = max_deviation(destination, [START, MAIN_LAST], lambda t, i: destination_start[i])
        check(keys_of(destination) == [float(START)] and still <= UNCHANGED_TOL,
              "destination: the curve keeps its one key and its points (keys %r, largest move %.3e)" % (keys_of(destination), still))
        corr_end = correspondence_at(link, MAIN_LAST)
        report("NOTE correspondence params: frame %d %r, frame %d %r" % (START, corr_start, MAIN_LAST, corr_end))
        check(abs(corr_end[1] - corr_start[1]) >= SLID_MIN,
              "correspondence: its destination end slid along the still destination curve (%.4f -> %.4f)" % (corr_start[1], corr_end[1]))
        check(abs(corr_end[0] - corr_start[0]) <= SLID_MIN,
              "correspondence: its source end stayed put on the source curve, which moved with the picture (%.4f -> %.4f)" % (corr_start[0], corr_end[0]))
        kept = scene.sw.getLink(source)
        check(kept is not None and kept.getDestination().getScriptName() == destination.getScriptName(),
              "link: the source curve is still linked to the destination curve")

        gt.section("scrub")
        rg.select_tool(gl)
        for frame in (START, (START + MAIN_LAST) // 2, MAIN_LAST):
            yield from go_to(frame)
            yield from settle()
            p = pos(source, 0, frame)
            c = correspondence_at(link, frame)
            shot("m220-splinewarp-follow-%d.png" % frame,
                 "SplineWarp frame %d: source point 0 at (%.1f, %.1f), correspondence params (%.3f, %.3f); the warped output follows the pan"
                 % (frame, p[0], p[1], c[0], c[1]))

    def steps():
        gt.section("setup")
        scene = yield from start_scene()
        yield from open_scene_warp(scene)
        yield from section_follow(scene)
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
