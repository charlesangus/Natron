# Builds the heavy roto/tracker project the project I/O benchmark saves and loads. Run inside NatronRenderer:
#
#   HEAVY_OUT=build/bench/heavy.ntpz NatronRenderer -b tools/bench/make_heavy_project.py
#
# Environment:
#   HEAVY_OUT      .ntpz to write (default build/bench/heavy.ntpz under the working directory)
#   HEAVY_SEED     seed of the generated geometry (default 1)
#   HEAVY_STROKES  open beziers standing in for paint strokes (default 200)
#   HEAVY_SAMPLES  control points per stroke (default 500)
#   HEAVY_BEZIERS  animated beziers (default 30)
#   HEAVY_POINTS   control points per bezier (default 24)
#   HEAVY_KEYS     keyframes per bezier (default 200)
#   HEAVY_EDITS    control points edited per key (default 1; 0 edits all of them, which is quadratic and takes
#                  about 90 s per bezier). Every key still stores all points and their feather points: the
#                  first edit at a new time keys the others at their interpolated values.
#   HEAVY_TRACKS   tracks (default 16)
#   HEAVY_FRAMES   keyframes per track (default 2000)
#   HEAVY_COMP     .ntpz loaded first as the node graph (default tools/bench/comps/keying_grade.ntpz, "" for none)
#
# The Python API cannot create paint strokes or pressure samples, so each stroke is an unfinished
# bezier whose control points are the stroke samples: the same per-sample positions and tangents,
# but no pressure and no per-sample time.

import math
import os
import random
import sys
import time
import traceback

import NatronEngine

OUT = os.environ.get("HEAVY_OUT", os.path.join(os.getcwd(), "build", "bench", "heavy.ntpz"))
SEED = int(os.environ.get("HEAVY_SEED", "1"))
STROKES = int(os.environ.get("HEAVY_STROKES", "200"))
SAMPLES = int(os.environ.get("HEAVY_SAMPLES", "500"))
BEZIERS = int(os.environ.get("HEAVY_BEZIERS", "30"))
POINTS = int(os.environ.get("HEAVY_POINTS", "24"))
KEYS = int(os.environ.get("HEAVY_KEYS", "200"))
EDITS = int(os.environ.get("HEAVY_EDITS", "1"))
TRACKS = int(os.environ.get("HEAVY_TRACKS", "16"))
FRAMES = int(os.environ.get("HEAVY_FRAMES", "2000"))
COMP = os.environ.get("HEAVY_COMP", os.path.join(os.getcwd(), "tools", "bench", "comps", "keying_grade.ntpz"))
WIDTH, HEIGHT = 3840.0, 2160.0


def log(msg):
    print("[heavy] " + msg)
    sys.stdout.flush()


def make(plugin_id, name):
    props = {"CreateNodeArgsPropNodeInitialName": NatronEngine.StringNodeCreationProperty(name)}
    node = session.createNode(plugin_id, -1, None, props)
    if node is None:
        raise RuntimeError("could not create " + plugin_id)
    return node


def build_strokes(roto, rng):
    for s in range(STROKES):
        x = rng.uniform(0.0, WIDTH)
        y = rng.uniform(0.0, HEIGHT)
        heading = rng.uniform(0.0, 2.0 * math.pi)
        stroke = roto.createBezier(x, y, 1)
        stroke.setScriptName("stroke%d" % s)
        for _ in range(SAMPLES - 1):
            heading += rng.uniform(-0.3, 0.3)
            x = min(max(x + 4.0 * math.cos(heading), 0.0), WIDTH)
            y = min(max(y + 4.0 * math.sin(heading), 0.0), HEIGHT)
            stroke.addControlPoint(x, y)


def build_beziers(roto, rng):
    for b in range(BEZIERS):
        cx = rng.uniform(400.0, WIDTH - 400.0)
        cy = rng.uniform(300.0, HEIGHT - 300.0)
        radius = rng.uniform(100.0, 300.0)
        shape = roto.createBezier(cx + radius, cy, 1)
        shape.setScriptName("shape%d" % b)
        for i in range(1, POINTS):
            angle = 2.0 * math.pi * i / POINTS
            shape.addControlPoint(cx + radius * math.cos(angle), cy + radius * math.sin(angle))
        shape.setCurveFinished(True)
        phases = [rng.uniform(0.0, 2.0 * math.pi) for _ in range(POINTS)]
        for k in range(KEYS):
            t = 1 + k
            # Each setPointAtIndex clones every point's curves, so its cost grows with the keys already set.
            for e in range(EDITS or POINTS):
                i = (k * 7 + e) % POINTS
                angle = 2.0 * math.pi * i / POINTS
                r = radius * (1.0 + 0.2 * math.sin(0.07 * t + phases[i]))
                px = cx + r * math.cos(angle) + 0.5 * t
                py = cy + r * math.sin(angle)
                tangent = r * 0.1
                shape.setPointAtIndex(i, t, px, py,
                                      -tangent * math.sin(angle), tangent * math.cos(angle),
                                      tangent * math.sin(angle), -tangent * math.cos(angle))
                fr = r + 20.0
                shape.setFeatherPointAtIndex(i, t, cx + fr * math.cos(angle) + 0.5 * t, cy + fr * math.sin(angle),
                                             0.0, 0.0, 0.0, 0.0)


def build_tracks(tracker, rng):
    for n in range(TRACKS):
        track = tracker.createTrack()
        track.setScriptName("track%d" % n)
        center = track.getParam("centerPoint")
        if center is None:
            raise RuntimeError("track has no centerPoint param")
        x = rng.uniform(200.0, WIDTH - 200.0)
        y = rng.uniform(200.0, HEIGHT - 200.0)
        for f in range(1, FRAMES + 1):
            x += rng.uniform(-1.5, 1.5)
            y += rng.uniform(-1.5, 1.5)
            center.setValueAtTime(x, f, 0)
            center.setValueAtTime(y, f, 1)


def main():
    global session
    rng = random.Random(SEED)
    session = app
    if COMP:
        loaded = app.loadProject(COMP)
        if loaded is None:
            raise RuntimeError("could not load " + COMP)
        session = loaded
    session.getProjectParam("frameRange").set(1, max(FRAMES, KEYS))

    t0 = time.time()
    roto_node = make("fr.inria.built-in.Roto", "heavy_roto")
    roto = roto_node.getRotoContext()
    build_strokes(roto, rng)
    log("%d strokes x %d samples in %.1fs" % (STROKES, SAMPLES, time.time() - t0))
    t0 = time.time()
    build_beziers(roto, rng)
    log("%d beziers x %d points x %d keys in %.1fs" % (BEZIERS, POINTS, KEYS, time.time() - t0))
    t0 = time.time()
    tracker_node = make("fr.inria.built-in.Tracker", "heavy_tracker")
    build_tracks(tracker_node.getTrackerContext(), rng)
    log("%d tracks x %d frames in %.1fs" % (TRACKS, FRAMES, time.time() - t0))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    if not session.saveProjectAs(OUT):
        raise RuntimeError("could not save " + OUT)
    log("wrote %s (%d bytes)" % (OUT, os.path.getsize(OUT)))


# A script run with -b renders the project's writers when it ends or fails, and the loaded comp's Write
# node points at a directory that does not exist; leaving through os._exit skips that on every path.
try:
    session = app
    main()
except BaseException:
    traceback.print_exc()
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(1)
sys.stdout.flush()
os._exit(0)
