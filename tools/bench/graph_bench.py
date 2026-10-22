# Render-scaling benchmark for large node graphs. Run inside NatronRenderer:
#
#   BENCH_TOPO=chain BENCH_N=100 BENCH_RES=hd NatronRenderer -b tools/bench/graph_bench.py
#
# Environment:
#   BENCH_TOPO    chain | mixed | wide | comp (generator-fed), or the plate-fed families
#                 readchain | footagecomp | iobound | rambound | deepcomp (see README "Realistic workloads"),
#                 or the per-node chains ccchain | blurchain | xfchain | mergechain | mergesrcchain,
#                 or the fixed GPU-region graphs gtg (Grade -> Transform with motion blur -> Grade) and
#                 gebg (Grade -> Erode -> Blur -> Grade), and node (BENCH_NODE alone) and gxg (Grade -> BENCH_NODE
#                 -> Grade); BENCH_N is ignored by these four
#   BENCH_NODE    the node of the node and gxg topologies: transform (animated, motion blur per BENCH_MOTION_BLUR)
#                 | blur (BENCH_BLUR_SIZE, BENCH_BLUR_FILTER) | erode (BENCH_ERODE_SIZE) | chromakeyer | edgedetect
#   BENCH_N       approximate number of processing nodes
#   BENCH_SIZE    WxH: render generator-fed topologies at this size; BENCH_RES is then only the record's label
#   BENCH_BLUR_SIZE  size parameter of the Blur nodes (default 3; sigma = size / 2.4)
#   BENCH_RES     tiny (32x32, isolates per-node overhead) | hd (1920x1080) | uhd (3840x2160); for the
#                 plate-fed families it also picks the plate resolution (deepcomp always uses HD)
#   BENCH_BLUR_FILTER  Blur "filter" choice (default: the plugin's own default)
#   BENCH_MOTION_BLUR  Transform motion blur quality in gtg and the transform BENCH_NODE (default 1)
#   BENCH_ERODE_SIZE  Erode size in gebg and the erode BENCH_NODE (default 6)
#   BENCH_PLATES_DIR  where make_plates.py wrote the plates (default build/bench/fixtures)
#   BENCH_FRAMES  frames timed one at a time after the warm-up frame (default 3)
#   BENCH_WARMUP  0 times the first frame too instead of rendering it as an untimed warm-up (default 1)
#   BENCH_RANGE   frames rendered as one range, to measure frame-parallel throughput (default 0)
#   BENCH_KEEP_OUTPUT  1 keeps the rendered EXR files; by default each render's files are deleted and the
#                 filesystem synced outside the timed interval, so dirty pages never throttle later frames
#   BENCH_OUT     JSON-lines file the result is appended to
#   BENCH_SEED    seed for the comp topology (default 1)
#   BENCH_SETTINGS  semicolon-separated name=value Natron settings, recorded as "settings"; the shell
#                 scripts pass them to NatronRenderer as --setting arguments (default empty)
#   BENCH_RENDER_STATS  1 saves the project after the timed frames and renders one frame of it with
#                 NatronRenderer --render-stats (app.render cannot enable stats); "Tasks run" and
#                 "Max concurrent tasks" from the -stats.txt file are recorded as tasks_run and
#                 max_concurrent_tasks. Needs the stats-capable CLI path, so it is a separate cold render.
#   BENCH_RENDERER  NatronRenderer binary for that render (default build/release/Renderer/NatronRenderer)
#   BENCH_IMPL    native (default) | ofx: which implementation of the IDs in NATIVE_MAJORS to create; the
#                 OFX plugin and its native replacement share an ID and differ by major version
#   BENCH_SESSION a | bc | playback: a session that counts one-time costs (no warm-up render, setup is part of the
#                 measurement) instead of the timed-frames loop. a renders BENCH_SESSION_FRAME once, labelled
#                 BENCH_LABEL (default a); bc renders it, then re-renders it BENCH_RERENDERS times (default 4)
#                 with a slightly changed knob in between (label b), then renders BENCH_JUMP_FRAME (default 7,
#                 label c); playback renders BENCH_PLAYBACK_FRAMES frames (default 4) from the session frame as one
#                 range. Each render is timed in-process around app.render, and its output is deleted and synced
#                 outside the timing. Needs BENCH_RENDER_DIR, an absolute existing directory.
#   BENCH_SESSION_FRAME  frame of the first render (default 2)
#   BENCH_COMP    path of an .ntpz to load instead of building a topology (BENCH_TOPO=ntp); its Write node is
#                 pointed at BENCH_RENDER_DIR, and BENCH_SIZE other than uhd adds a project format of that size
#   BENCH_HOLD    seconds to sleep before rendering, so a sampler can attach (default 0)
#   BENCH_DRY_RUN 1 builds the graph against stand-in objects and prints the planned nodes without
#                 Natron (python3 tools/bench/graph_bench.py on the host); nothing is rendered

import glob
import json
import os
import random
import re
import resource
import statistics
import subprocess
import sys
import time

DRY_RUN = os.environ.get("BENCH_DRY_RUN") == "1"
if DRY_RUN:
    class _Param(object):
        def __init__(self):
            self.value = None

        def set(self, *values):
            self.value = values[0]

        def get(self):
            return self.value

        def getOption(self, value):
            return value

        def setValueAtTime(self, *args):
            pass

    class _Shapes(object):
        def createEllipse(self, *args):
            return True

        def createRectangle(self, *args):
            return True

    class _Node(object):
        def __init__(self, plugin_id):
            self.plugin_id = plugin_id
            self.params = {}

        def getParam(self, name):
            return self.params.setdefault(name, _Param())

        def connectInput(self, index, node):
            pass

        def getMaxInputCount(self):
            return 4

        def getInputLabel(self, index):
            return ("B", "A", "Mask", "A_2")[index]

        def getRotoContext(self):
            return _Shapes()

        def getScriptName(self):
            return self.plugin_id

        def getPluginID(self):
            return self.plugin_id

    class _App(object):
        def createNode(self, plugin_id, *args):
            return _Node(plugin_id)

        def createReader(self, path, *args):
            return _Node("fr.inria.built-in.Read")

        def createWriter(self, path, *args):
            return _Node("fr.inria.built-in.Write")

        def getProjectParam(self, name):
            return _Param()

    class NatronEngine(object):
        StringNodeCreationProperty = staticmethod(lambda value: value)

    app = _App()
else:
    import NatronEngine

TOPO = os.environ.get("BENCH_TOPO", "chain")
N = int(os.environ.get("BENCH_N", "10"))
RES = os.environ.get("BENCH_RES", "tiny")
FRAMES = int(os.environ.get("BENCH_FRAMES", "3"))
WARMUP = os.environ.get("BENCH_WARMUP", "1") != "0"
NODE = os.environ.get("BENCH_NODE", "transform")
RANGE = int(os.environ.get("BENCH_RANGE", "0"))
OUT = os.environ.get("BENCH_OUT", "")
KEEP_OUTPUT = os.environ.get("BENCH_KEEP_OUTPUT") == "1"
SEED = int(os.environ.get("BENCH_SEED", "1"))
SESSION = os.environ.get("BENCH_SESSION", "")
SESSION_FRAME = int(os.environ.get("BENCH_SESSION_FRAME", "2"))
JUMP_FRAME = int(os.environ.get("BENCH_JUMP_FRAME", "7"))
RERENDERS = int(os.environ.get("BENCH_RERENDERS", "4"))
PLAYBACK_FRAMES = int(os.environ.get("BENCH_PLAYBACK_FRAMES", "4"))
LABEL = os.environ.get("BENCH_LABEL", "a")
RENDER_DIR = os.environ.get("BENCH_RENDER_DIR", "")
COMP = os.environ.get("BENCH_COMP", "")
if SESSION not in ("", "a", "bc", "playback"):
    raise SystemExit("BENCH_SESSION must be a, bc or playback, got %r" % SESSION)
BLUR_FILTER = os.environ.get("BENCH_BLUR_FILTER", "")
HOLD = float(os.environ.get("BENCH_HOLD", "0"))
SETTINGS = os.environ.get("BENCH_SETTINGS", "")
WORK = os.environ.get("BENCH_WORK", os.path.join(os.getcwd(), "build", "bench", "work"))
PLATES = os.environ.get("BENCH_PLATES_DIR") or os.path.join(os.getcwd(), "build", "bench", "fixtures")
# Explicit names bypass the default unique-name search; BENCH_NAMED=0 measures that path.
NAMED = os.environ.get("BENCH_NAMED", "1") != "0"
IMPL = os.environ.get("BENCH_IMPL", "native")
if IMPL not in ("ofx", "native"):
    raise SystemExit("BENCH_IMPL must be ofx or native, got %r" % IMPL)

# plugin ID -> (OFX major, native major). A native node takes over its OFX ID at the next major, so
# an unversioned request already resolves to it; only the benchmark needs to pick one explicitly.
NATIVE_MAJORS = {
    "net.sf.openfx.GradePlugin": (2, 3),
}

LAST_FRAME = 1 + FRAMES + max(RANGE, 0) + 1
if SESSION:
    LAST_FRAME = max(LAST_FRAME, JUMP_FRAME, SESSION_FRAME + PLAYBACK_FRAMES) + 1

READ_TOPOS = ("readchain", "footagecomp", "iobound", "deepcomp")
PLATE_RES = "hd" if TOPO == "deepcomp" else RES
SIZES = {"tiny": (32, 32), "hd": (1920, 1080), "uhd": (3840, 2160)}
# BENCH_SIZE=WxH renders generator-fed topologies at that size; BENCH_RES then only labels the record.
CUSTOM_SIZE = os.environ.get("BENCH_SIZE", "")
if CUSTOM_SIZE:
    SIZES[RES] = tuple(int(v) for v in CUSTOM_SIZE.lower().split("x"))
# Blur "size" (diameter): the Gaussian standard deviation is size / 2.4.
BLUR_SIZE = float(os.environ.get("BENCH_BLUR_SIZE", "3"))
MOTION_BLUR = float(os.environ.get("BENCH_MOTION_BLUR", "1"))
ERODE_SIZE = int(os.environ.get("BENCH_ERODE_SIZE", "6"))

counts = {}
majors = {}
# Per frame, the plate file bytes the graph decodes: one entry per Read/DeepRead node.
plate_reads = []


def log(msg):
    print("[bench] " + msg)
    sys.stdout.flush()


def make(plugin_id):
    pair = NATIVE_MAJORS.get(plugin_id)
    major = -1 if pair is None else pair[0 if IMPL == "ofx" else 1]
    majors[plugin_id] = major
    if NAMED:
        name = "n%d" % sum(counts.values())
        props = {"CreateNodeArgsPropNodeInitialName": NatronEngine.StringNodeCreationProperty(name)}
        node = app.createNode(plugin_id, major, None, props)
    else:
        node = app.createNode(plugin_id, major)
    if node is None:
        raise RuntimeError("could not create " + plugin_id)
    counts[plugin_id] = counts.get(plugin_id, 0) + 1
    return node


def param(node, name):
    p = node.getParam(name)
    if p is None:
        raise RuntimeError("%s has no param %s" % (node.getPluginID(), name))
    return p


def set_choice(node, name, value):
    p = param(node, name)
    p.set(value)
    shown = p.getOption(p.get()) if hasattr(p, "getOption") else p.get()
    if str(shown).lower() != value.lower():
        log("%s.%s is %s after setting %s" % (node.getScriptName(), name, shown, value))


def sequence(stem):
    # Plate sequences are stem.####.exr in PLATES, written by make_plates.py.
    pattern = os.path.join(PLATES, stem + ".####.exr")
    files = sorted(glob.glob(os.path.join(PLATES, stem + ".[0-9][0-9][0-9][0-9].exr")))
    if not files and not DRY_RUN:
        raise RuntimeError("no plate %s; run tools/bench/make_plates.py first" % pattern)
    sizes = [os.path.getsize(f) for f in files]
    return pattern, len(files), (sum(sizes) // len(sizes) if sizes else 0)


def read(index):
    # index cycles the three plates; looping keeps every timed frame a distinct decode.
    pattern, n_frames, frame_bytes = sequence("plate_%s_%d" % (PLATE_RES, index % 3 + 1))
    if NAMED:
        name = "n%d" % sum(counts.values())
        props = {"CreateNodeArgsPropNodeInitialName": NatronEngine.StringNodeCreationProperty(name)}
        node = app.createReader(pattern, None, props)
    else:
        node = app.createReader(pattern)
    if node is None:
        raise RuntimeError("could not create a reader for " + pattern)
    counts["fr.inria.built-in.Read"] = counts.get("fr.inria.built-in.Read", 0) + 1
    for name in ("before", "after"):
        if node.getParam(name) is not None:
            set_choice(node, name, "loop")
        else:
            log("reader has no %s param; frames past %d may hold" % (name, n_frames))
    plate_reads.append(frame_bytes)
    return node


def deep_read():
    pattern, n_frames, frame_bytes = sequence("deep_hd")
    if n_frames and n_frames < LAST_FRAME:
        raise RuntimeError("deepcomp renders frames 1-%d but %s has %d frames; DeepRead cannot loop, so "
                           "regenerate with BENCH_PLATE_FRAMES=%d or lower BENCH_FRAMES/BENCH_RANGE"
                           % (LAST_FRAME, pattern, n_frames, LAST_FRAME))
    node = make("fr.natron.DeepRead")
    param(node, "filename").set(pattern)
    plate_reads.append(frame_bytes)
    return node


def source(index):
    # Animated so that nothing downstream is treated as frame-invariant and cached once.
    cb = make("net.sf.openfx.CheckerBoardPlugin")
    if RES == "tiny" or CUSTOM_SIZE:
        param(cb, "extent").set("Size")
        param(cb, "size").set(*SIZES[RES])
    else:
        param(cb, "extent").set("Project")
    color = param(cb, "color0")
    for f in range(1, LAST_FRAME + 1):
        color.setValueAtTime(0.1 + 0.01 * f + 0.001 * index, f, 0)
    return cb


def grade(inp, i):
    n = make("net.sf.openfx.GradePlugin")
    n.connectInput(0, inp)
    param(n, "multiply").set(1.0 + 0.0001 * (i % 7 + 1), 1.0, 1.0, 1.0)
    return n


def colorcorrect(inp, i):
    n = make("net.sf.openfx.ColorCorrectPlugin")
    n.connectInput(0, inp)
    param(n, "MasterSaturation").set(1.0 + 0.001 * (i % 5 + 1), 1.0, 1.0, 1.0)
    return n


def transform(inp, i):
    n = make("net.sf.openfx.TransformPlugin")
    n.connectInput(0, inp)
    param(n, "translate").set(0.25 + 0.01 * (i % 3), 0.0)
    return n


def blur(inp, i):
    n = make("net.sf.cimg.CImgBlur")
    n.connectInput(0, inp)
    param(n, "size").set(BLUR_SIZE, BLUR_SIZE)
    if BLUR_FILTER:
        set_choice(n, "filter", BLUR_FILTER)
    return n


def merge(a, b, mask=None):
    n = make("net.sf.openfx.MergePlugin")
    n.connectInput(0, b)
    n.connectInput(1, a)
    param(n, "mix").set(0.5)
    if mask is not None:
        idx = [i for i in range(n.getMaxInputCount()) if n.getInputLabel(i) == "Mask"]
        if not idx:
            raise RuntimeError("Merge has no Mask input")
        n.connectInput(idx[0], mask)
        param(n, "enableMask_Mask").set(True)
    return n


def moving(inp, i):
    # A small per-frame translate, like a stabilised plate drifting, so no frame repeats another.
    n = make("net.sf.openfx.TransformPlugin")
    n.connectInput(0, inp)
    t = param(n, "translate")
    for f in range(1, LAST_FRAME + 1):
        t.setValueAtTime(0.5 * f + 0.25 * (i % 4), f, 0)
        t.setValueAtTime(0.25 * f, f, 1)
    return n


def roto(i):
    n = make("fr.inria.built-in.Roto")
    ctx = n.getRotoContext()
    if ctx is None:
        raise RuntimeError("Roto node has no Python roto context")
    w, h = SIZES[PLATE_RES]
    if i % 2 == 0:
        shape = ctx.createEllipse(w * 0.5, h * 0.5, h * 0.6, True, 1)
    else:
        shape = ctx.createRectangle(w * 0.2, h * 0.8, h * 0.5, 1)
    if shape is None:
        raise RuntimeError("could not create a roto shape")
    return n


def dot(inp):
    n = make("fr.inria.built-in.Dot")
    n.connectInput(0, inp)
    return n


def build_chain():
    out = source(0)
    for i in range(N):
        out = grade(out, i)
    return out


def build_ccchain():
    out = source(0)
    for i in range(N):
        out = colorcorrect(out, i)
    return out


def build_blurchain():
    out = source(0)
    for i in range(N):
        out = blur(out, i)
    return out


def build_xfchain():
    # Transforms in series would concatenate into one resample, so each is followed by a Grade;
    # subtract the chain topology's per-node cost to get the Transform's.
    out = source(0)
    for i in range(N):
        out = grade(transform(out, i), i)
    return out


def build_mergechain():
    src = source(1)
    out = source(0)
    for i in range(N):
        out = merge(src, out)
    return out


def build_mergesrcchain():
    # One more CheckerBoard per Merge than mergechain: the difference is the generator's cost.
    out = source(0)
    for i in range(N):
        out = merge(source(i + 1), out)
    return out


def animated_transform(inp):
    xf = make("net.sf.openfx.TransformPlugin")
    xf.connectInput(0, inp)
    w, h = SIZES[RES]
    param(xf, "center").set(w / 2.0, h / 2.0)
    # Motion blur samples the transform across the shutter, so it must change from frame to frame.
    translate = param(xf, "translate")
    rotate = param(xf, "rotate")
    for f in range(1, LAST_FRAME + 1):
        translate.setValueAtTime(14.0 * f, f, 0)
        translate.setValueAtTime(6.0 * f, f, 1)
        rotate.setValueAtTime(0.8 * f, f, 0)
    param(xf, "motionBlur").set(MOTION_BLUR)
    param(xf, "shutter").set(1.0)
    return xf


def build_gtg():
    return grade(animated_transform(grade(source(0), 0)), 1)


def erode(inp):
    n = make("net.sf.cimg.CImgErode")
    n.connectInput(0, inp)
    param(n, "size").set(ERODE_SIZE, ERODE_SIZE)
    return n


def chromakeyer(inp):
    n = make("net.sf.openfx.ChromaKeyerPlugin")
    n.connectInput(0, inp)
    return n


def edgedetect(inp):
    n = make("eu.cimg.EdgeDetect")
    n.connectInput(0, inp)
    return n


# The node under test and the first Grade of a gxg graph, for the knob the session changes between renders.
tweak_targets = {}


def bench_node(inp):
    makers = {"transform": animated_transform, "blur": lambda i: blur(i, 0), "erode": erode,
              "chromakeyer": chromakeyer, "edgedetect": edgedetect}
    if NODE not in makers:
        raise RuntimeError("BENCH_NODE must be one of %s, got %r" % (", ".join(sorted(makers)), NODE))
    node = makers[NODE](inp)
    tweak_targets["node"] = node
    return node


def build_node():
    return bench_node(source(0))


def build_gxg():
    first = grade(source(0), 0)
    tweak_targets["grade"] = first
    return grade(bench_node(first), 1)


def build_gebg():
    return grade(blur(erode(grade(source(0), 0)), 0), 1)


def build_mixed():
    ops = (grade, blur, transform, colorcorrect)
    out = source(0)
    for i in range(N):
        out = ops[i % len(ops)](out, i)
    return out


def merge_tree(level, masked=0):
    # Pairwise reduction; the first `masked` merges made (the leaf level first) get a Roto mask.
    made = 0
    while len(level) > 1:
        nxt = []
        for j in range(0, len(level) - 1, 2):
            mask = roto(made) if made < masked else None
            nxt.append(merge(level[j], level[j + 1], mask))
            made += 1
        if len(level) % 2:
            nxt.append(level[-1])
        level = nxt
    return level[0]


def build_wide():
    # Leaves of (source -> grade), reduced by a balanced tree of merges: about 3 nodes per leaf.
    leaves = max(2, (N + 1) // 3)
    return merge_tree([grade(source(i), i) for i in range(leaves)])


def build_rambound():
    # The wide shape meant for UHD: with N=192 its 64 leaf outputs of 133 MB each (float RGBA)
    # add up to more than the cache budget of a 15 GB machine.
    if RES != "uhd":
        log("rambound is meant for BENCH_RES=uhd, running at %s" % RES)
    return build_wide()


def build_readchain():
    out = read(0)
    for i in range(N):
        out = grade(out, i)
    return out


def build_footagecomp():
    # K plates, each graded, drifting and (every other one) blurred, merged pairwise; the first
    # half of the merges are held out by a Roto matte, as a comp's leaf merges usually are.
    k = max(2, N // 4)
    branches = []
    for i in range(k):
        b = moving(grade(read(i), i), i)
        if i % 2 == 0:
            b = blur(b, i)
        branches.append(b)
    return merge_tree(branches, masked=(k - 1) // 2)


def build_iobound():
    k = max(2, (N + 1) // 2)
    out = read(0)
    for i in range(1, k):
        out = merge(read(i), out)
    return out


def build_deepcomp():
    k = max(2, N // 2)
    out = deep_read()
    for _ in range(1, k):
        m = make("fr.natron.DeepMerge")
        m.connectInput(0, deep_read())
        m.connectInput(1, out)
        out = m
    flat = make("fr.natron.DeepToImage")
    flat.connectInput(0, out)
    return merge(flat, read(0))


def build_comp():
    # A seeded random DAG shaped like a comp: branches that are graded, transformed, blurred,
    # split to several consumers and merged back together.
    rng = random.Random(SEED)
    streams = [source(0)]
    made = 0
    n_sources = 1
    while made < N:
        r = rng.random()
        k = rng.randrange(len(streams))
        s = streams[k]
        if r < 0.08:
            streams.append(source(n_sources))
            n_sources += 1
        elif r < 0.38:
            streams[k] = grade(s, made)
        elif r < 0.48:
            streams[k] = colorcorrect(s, made)
        elif r < 0.58:
            streams[k] = transform(s, made)
        elif r < 0.66:
            streams[k] = blur(s, made)
        elif r < 0.76:
            # Fan-out: the same node feeds two branches.
            streams.append(dot(s))
        elif len(streams) > 1:
            j = rng.randrange(len(streams))
            if j != k:
                m = merge(streams[k], streams[j])
                streams[k] = m
                del streams[j]
            else:
                streams[k] = grade(s, made)
        else:
            streams[k] = grade(s, made)
        made += 1
    out = streams[0]
    for s in streams[1:]:
        out = merge(s, out)
    return out


# Knob changed between re-renders: node kind -> (param, value for the k-th change). Every value is new, so
# a content-hash cache can never serve a re-render from an earlier one.
NODE_TWEAKS = {
    "transform": ("scale", lambda k: (1.0 + 0.001 * k, 1.0 + 0.001 * k)),
    "blur": ("size", lambda k: (BLUR_SIZE * (1.0 + 0.002 * k),) * 2),
    "erode": ("size", lambda k: (ERODE_SIZE + k,) * 2),
    "chromakeyer": ("acceptanceAngle", lambda k: (60.0 + 0.5 * k,)),
    "edgedetect": ("blurSize", lambda k: (1.0 + 0.01 * k,)),
}
COMP_TWEAKS = {
    "keying_grade": ("fg_grade1", "multiply", (1.05, 1.0, 0.95, 1.0)),
    "cg_multipass": ("glow_thresh", "offset", (-0.4, -0.4, -0.4, 0.0)),
    "defocus_retime": ("plate_grade", "multiply", (1.1, 1.0, 0.9, 1.0)),
}


def make_tweak(session_app):
    """Returns a function k -> None that changes the knob for the k-th re-render."""
    if COMP:
        comp = os.path.splitext(os.path.basename(COMP))[0]
        node_name, name, base = COMP_TWEAKS[comp]
        prm = param(session_app.getNode(node_name), name)
        return lambda k: prm.set(base[0] + 0.001 * k, *base[1:])
    if TOPO == "node" or TOPO == "gxg":
        node = tweak_targets["node"]
        name, value = NODE_TWEAKS[NODE]
        if node.getParam(name) is not None:
            prm = node.getParam(name)
            return lambda k: prm.set(*value(k))
        log("%s has no param %s; changing the first Grade instead" % (node.getPluginID(), name))
    if "grade" not in tweak_targets:
        raise RuntimeError("no knob to change for %s" % TOPO)
    prm = param(tweak_targets["grade"], "multiply")
    return lambda k: prm.set(1.0001 + 0.001 * k, 1.0, 1.0, 1.0)


def load_comp(path, render_dir):
    """Loads the .ntpz and points its Write node at render_dir; returns (app, writer)."""
    loaded = app.loadProject(path)
    session_app = loaded if loaded is not None else app
    if CUSTOM_SIZE and CUSTOM_SIZE != "3840x2160":
        w, h = (int(v) for v in CUSTOM_SIZE.lower().split("x"))
        session_app.addFormat("bench%dx%d %d %d 1" % (w, h, w, h))
        session_app.getProjectParam("outputFormat").set("bench%dx%d" % (w, h))
    writer = session_app.getNode("Write")
    if writer is None:
        raise RuntimeError("%s has no Write node" % path)
    stem = os.path.splitext(os.path.basename(path))[0]
    global written_glob
    written_glob = os.path.join(render_dir, stem + ".[0-9][0-9][0-9][0-9].exr")
    param(writer, "filename").set(os.path.join(render_dir, stem + ".####.exr"))
    return session_app, writer


def run_session(session_app, writer):
    """Renders per BENCH_SESSION. The first render includes every one-time cost; nothing is warmed up first."""
    marks = []
    sequence_no = [0]

    def timed(label, first, last):
        wall, cpu, io = render(writer, first, last)
        count = last - first + 1
        marks.append({"label": label, "index": len(marks), "frame": first, "frames": count,
                      "profileFrame": sequence_no[0], "wall_s": round(wall, 4), "cpu_s": round(cpu, 4)})
        sequence_no[0] += count
        log("render %d %s frames %d-%d wall %.4fs" % (len(marks) - 1, label, first, last, wall))

    if SESSION == "a":
        timed(LABEL, SESSION_FRAME, SESSION_FRAME)
    elif SESSION == "bc":
        tweak = make_tweak(session_app)
        timed("first", SESSION_FRAME, SESSION_FRAME)
        for k in range(1, RERENDERS + 1):
            tweak(k)
            timed("b", SESSION_FRAME, SESSION_FRAME)
        timed("c", JUMP_FRAME, JUMP_FRAME)
    else:
        timed("playback", SESSION_FRAME, SESSION_FRAME + PLAYBACK_FRAMES - 1)
    return marks


def rusage():
    ru = resource.getrusage(resource.RUSAGE_SELF)
    return ru.ru_utime + ru.ru_stime


def vm(field):
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith(field + ":"):
                return int(line.split()[1]) // 1024
    return -1


IO_FIELDS = (("rchar", "io_rchar"), ("wchar", "io_wchar"), ("read_bytes", "io_read_bytes"),
             ("write_bytes", "io_write_bytes"))


def proc_io():
    # /proc/self/io sums every thread of the process, so decoder and writer threads count too.
    vals = {}
    try:
        with open("/proc/self/io") as f:
            for line in f:
                k, v = line.split(":")
                vals[k.strip()] = int(v)
    except (OSError, ValueError):
        return None
    vals["majflt"] = resource.getrusage(resource.RUSAGE_SELF).ru_majflt
    return vals


def io_delta(before, after):
    if before is None or after is None:
        return dict((field, None) for _, field in IO_FIELDS + (("majflt", "majflt"),))
    out = dict((field, after[k] - before[k]) for k, field in IO_FIELDS)
    out["majflt"] = after["majflt"] - before["majflt"]
    return out


def io_sum(deltas):
    if not deltas:
        return io_delta(None, None)
    total = {}
    for d in deltas:
        for k, v in d.items():
            total[k] = None if v is None or total.get(k, 0) is None else total.get(k, 0) + v
    return total


PRESSURE_RESOURCES = ("cpu", "io", "memory")


def pressure_some10(resource_name):
    # None when the kernel has no PSI or the sandbox hides it.
    try:
        with open("/proc/pressure/" + resource_name) as f:
            for line in f:
                m = re.match(r"some .*avg10=([0-9.]+)", line)
                if m:
                    return float(m.group(1))
    except (OSError, ValueError):
        pass
    return None


def load_pressure():
    try:
        with open("/proc/loadavg") as f:
            load1 = float(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        load1 = None
    out = {"load1": load1}
    for name in PRESSURE_RESOURCES:
        out["psi_%s_some10" % name] = pressure_some10(name)
    return out


written_glob = ""
output_bytes = 0
output_files = 0


def settle_output():
    # Deleting before the sync drops the dirty pages instead of writing them back.
    global output_bytes, output_files
    for p in glob.glob(written_glob):
        output_bytes += os.path.getsize(p)
        output_files += 1
        if not KEEP_OUTPUT:
            os.remove(p)
    os.sync()


def render(writer, first, last):
    io0 = proc_io()
    c0, t0 = rusage(), time.time()
    app.render(writer, first, last)
    t1, c1 = time.time(), rusage()
    io = io_delta(io0, proc_io())
    settle_output()
    return t1 - t0, c1 - c0, io


def mem_total_mb():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) // 1024
    except OSError:
        pass
    return None


def render_stats(writer):
    # The CLI path (-w) honours --render-stats; the Python app.render does not.
    proj = os.path.join(WORK, "stats_%s_%d.ntpz" % (TOPO, N))
    app.saveProject(proj)
    exe = os.environ.get("BENCH_RENDERER", os.path.join(os.getcwd(), "build", "release", "Renderer", "NatronRenderer"))
    cmd = [exe]
    for s in SETTINGS.split(";"):
        if s:
            cmd += ["--setting", s]
    frame = 2
    cmd += ["--render-stats", "-w", writer.getScriptName(), "%d-%d" % (frame, frame), proj]
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    log("stats render exit=%d" % out.returncode)
    stats = os.path.join(WORK, "%s_%d_%s_%04d-stats.txt" % (TOPO, N, RES, frame))
    found = {}
    try:
        text = open(stats).read()
    except OSError:
        log("no stats file at %s; output tail: %s" % (stats, out.stdout[-300:]))
        return found
    for key, field in (("Tasks run", "tasks_run"), ("Max concurrent tasks", "max_concurrent_tasks")):
        m = re.search(r"^%s: (\d+)" % key, text, re.M)
        if m:
            found[field] = int(m.group(1))
    return found


def main_session():
    if not SESSION:
        raise RuntimeError("BENCH_TOPO=ntp needs BENCH_SESSION")
    if not DRY_RUN and not (RENDER_DIR and os.path.isabs(RENDER_DIR) and os.path.isdir(RENDER_DIR)):
        raise RuntimeError("BENCH_RENDER_DIR must be an absolute existing directory, got %r" % RENDER_DIR)
    global written_glob
    t0 = time.time()
    if TOPO == "ntp":
        if not COMP:
            raise RuntimeError("BENCH_TOPO=ntp needs BENCH_COMP")
        if DRY_RUN:
            log("dry run: session %s comp %s size %s" % (SESSION, COMP, CUSTOM_SIZE or "project"))
            return
        session_app, writer = load_comp(COMP, RENDER_DIR)
    else:
        session_app = app
        fmt = RES
        if fmt == "hd":
            app.getProjectParam("outputFormat").set("HD")
        elif fmt == "uhd":
            app.getProjectParam("outputFormat").set("UHD_4K")
        app.getProjectParam("frameRange").set(1, LAST_FRAME)
        root = {"node": build_node, "gxg": build_gxg}[TOPO]()
        if DRY_RUN:
            log("dry run: session %s %s nodes=%d %s" % (SESSION, TOPO, sum(counts.values()), json.dumps(counts, sort_keys=True)))
            make_tweak(app)(1)
            return
        written_glob = os.path.join(RENDER_DIR, "%s_%s_[0-9][0-9][0-9][0-9].exr" % (TOPO, RES))
        writer = app.createWriter(os.path.join(RENDER_DIR, "%s_%s_####.exr" % (TOPO, RES)))
        writer.connectInput(0, root)
        working = writer.getParam("ocioWorkingSpace").get()
        writer.getParam("ocioOutputSpace").set(working)
        writer.getParam("ocioOutputSpaceSet").set(True)
        comp_param = writer.getParam("compression")
        if comp_param is not None:
            comp_param.set("none")
    setup_s = time.time() - t0
    log("session %s setup %.2fs" % (SESSION, setup_s))
    pressure_start = load_pressure()
    marks = run_session(session_app, writer)
    result = {
        "topo": TOPO, "comp": os.path.basename(COMP) if COMP else None, "bench_node": NODE if TOPO in ("node", "gxg") else None,
        "res": RES, "size": CUSTOM_SIZE, "session": SESSION, "label": LABEL, "setup_s": round(setup_s, 4),
        "renders": marks, "settings": SETTINGS, "rss_peak_mb": vm("VmHWM"), "cpus": os.cpu_count(),
        "mem_total_mb": mem_total_mb(), "output_bytes": output_bytes, "output_frames": output_files,
        "load1_start": pressure_start["load1"],
    }
    line = json.dumps(result)
    log("RESULT " + line)
    if OUT:
        with open(OUT, "a") as f:
            f.write(line + "\n")
    marks_path = os.environ.get("BENCH_MARKS", "")
    if marks_path:
        with open(marks_path, "w") as f:
            for m in marks:
                f.write(json.dumps(m) + "\n")


def main():
    if SESSION or TOPO == "ntp":
        main_session()
        return
    if TOPO in READ_TOPOS and PLATE_RES not in ("hd", "uhd"):
        raise RuntimeError("%s reads plates and needs BENCH_RES=hd or uhd" % TOPO)
    fmt = PLATE_RES if TOPO in READ_TOPOS else RES
    if fmt == "hd":
        app.getProjectParam("outputFormat").set("HD")
    elif fmt == "uhd":
        app.getProjectParam("outputFormat").set("UHD_4K")
    app.getProjectParam("frameRange").set(1, LAST_FRAME)

    builders = {"chain": build_chain, "mixed": build_mixed, "wide": build_wide, "comp": build_comp,
                "readchain": build_readchain, "footagecomp": build_footagecomp, "iobound": build_iobound,
                "rambound": build_rambound, "deepcomp": build_deepcomp, "ccchain": build_ccchain,
                "blurchain": build_blurchain, "xfchain": build_xfchain, "mergechain": build_mergechain,
                "mergesrcchain": build_mergesrcchain, "gtg": build_gtg, "gebg": build_gebg,
                "node": build_node, "gxg": build_gxg}
    t0 = time.time()
    root = builders[TOPO]()
    build_s = time.time() - t0
    if DRY_RUN:
        log("dry run: %s N=%d nodes=%d %s" % (TOPO, N, sum(counts.values()), json.dumps(counts, sort_keys=True)))
        log("dry run: impl=%s majors %s" % (IMPL, json.dumps(majors, sort_keys=True)))
        log("dry run: plate bytes per frame %d over %d reads" % (sum(plate_reads), len(plate_reads)))
        return

    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, "%s_%d_%s_####.exr" % (TOPO, N, RES))
    written = os.path.join(WORK, "%s_%d_%s_[0-9][0-9][0-9][0-9].exr" % (TOPO, N, RES))
    global written_glob
    written_glob = written
    for stale in glob.glob(written):
        os.remove(stale)
    writer = app.createWriter(path)
    writer.connectInput(0, root)
    working = writer.getParam("ocioWorkingSpace").get()
    writer.getParam("ocioOutputSpace").set(working)
    writer.getParam("ocioOutputSpaceSet").set(True)
    for name, value in (("compression", "none"),):
        p = writer.getParam(name)
        if p is not None:
            try:
                p.set(value)
            except Exception as e:
                log("could not set writer %s: %s" % (name, e))
    comp = writer.getParam("compression")
    shown = None
    if comp is not None:
        shown = comp.getOption(comp.get()) if hasattr(comp, "getOption") else comp.get()
    log("writer compression=%s" % shown)

    n_nodes = sum(counts.values())
    log("built %s N=%d nodes=%d in %.2fs" % (TOPO, N, n_nodes, build_s))
    if HOLD > 0:
        log("holding %.0fs, pid %d" % (HOLD, os.getpid()))
        time.sleep(HOLD)

    rss_before = vm("VmRSS")
    pressure_start = load_pressure()
    warm_wall = warm_cpu = warm_io = None
    if WARMUP:
        warm_wall, warm_cpu, warm_io = render(writer, 1, 1)
    first_timed = 2 if WARMUP else 1
    walls, cpus, ios = [], [], []
    for f in range(first_timed, first_timed + FRAMES):
        w, c, io = render(writer, f, f)
        walls.append(w)
        cpus.append(c)
        ios.append(io)
    range_wall = range_cpu = range_io = None
    if RANGE > 0:
        first = first_timed + FRAMES
        range_wall, range_cpu, range_io = render(writer, first, first + RANGE - 1)
    pressure_end = load_pressure()

    stats_fields = render_stats(writer) if os.environ.get("BENCH_RENDER_STATS") == "1" else {}
    med = statistics.median(walls) if walls else None
    result = {
        "topo": TOPO,
        "n": N,
        "nodes": n_nodes,
        "res": RES,
        "size": CUSTOM_SIZE,
        "blur_size": BLUR_SIZE,
        "blur_filter": BLUR_FILTER,
        "named": NAMED,
        "build_s": round(build_s, 4),
        "warm_wall_s": round(warm_wall, 4) if warm_wall is not None else None,
        "warm_cpu_s": round(warm_cpu, 4) if warm_cpu is not None else None,
        "bench_node": NODE if TOPO in ("node", "gxg") else None,
        "erode_size": ERODE_SIZE,
        "motion_blur": MOTION_BLUR,
        "frame_wall_s": [round(w, 4) for w in walls],
        "frame_wall_med_s": round(med, 4) if med is not None else None,
        "frame_cpu_s": [round(c, 4) for c in cpus],
        "parallelism": round(sum(cpus) / sum(walls), 2) if walls and sum(walls) > 0 else None,
        "range_frames": RANGE,
        "range_wall_s": round(range_wall, 4) if range_wall is not None else None,
        "range_cpu_s": round(range_cpu, 4) if range_cpu is not None else None,
        "range_parallelism": round(range_cpu / range_wall, 2) if range_wall else None,
        "settings": SETTINGS,
        "rss_before_mb": rss_before,
        "rss_peak_mb": vm("VmHWM"),
        "counts": counts,
        "impl": IMPL,
        "majors": majors,
        "pixels": SIZES[fmt][0] * SIZES[fmt][1] if fmt in SIZES else None,
        "cpus": os.cpu_count(),
        "mem_total_mb": mem_total_mb(),
        "plate_res": PLATE_RES if TOPO in READ_TOPOS else None,
        "plate_bytes": sum(plate_reads),
        "output_bytes": output_bytes,
        "output_frames": output_files,
        "io_phases": {"warm": warm_io, "frames": io_sum(ios), "range": range_io},
    }
    for key, value in pressure_start.items():
        result[key if key != "load1" else "load1_start"] = value
    for key, value in pressure_end.items():
        result[(key if key != "load1" else "load1") + "_end"] = value
    # The top-level IO fields cover the timed frames, like frame_wall_s.
    result.update(io_sum(ios))
    result.update(stats_fields)
    line = json.dumps(result)
    log("RESULT " + line)
    if OUT:
        with open(OUT, "a") as f:
            f.write(line + "\n")


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    sys.stdout.flush()
    os._exit(3)
os._exit(0)
