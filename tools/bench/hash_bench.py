# Hash-cost benchmark: how long one knob edit takes to reach every downstream content hash.
# Run inside NatronRenderer, with the same graph selection as graph_bench.py:
#
#   BENCH_TOPO=comp BENCH_N=200 BENCH_RES=hd NatronRenderer -b tools/bench/hash_bench.py
#   BENCH_TOPO=roto BENCH_ROTO_SHAPES=200 BENCH_ROTO_POINTS=16 NatronRenderer -b tools/bench/hash_bench.py
#
# A "keystroke" is one setValue on a node at the top of the graph, followed by a read of the content
# hash of that node and of every node downstream of it, as a viewer render would read them. Hashes
# are lazy: the setValue only marks the edited node dirty, and the reads mark its dependents and
# recompute every dirty hash. The two are timed apart: "setvalue" from the call until it returns,
# "recompute" the reads that follow it. Nothing renders in background mode, so the setValue time is
# the mark plus the fixed cost of the setValue path itself. That fixed cost (knobChanged, undo
# bookkeeping, Python binding) is measured with a control edit of a hash-exempt knob (the node's
# user label) on the same node, and subtracted: "net" is the cost of the mark alone. The recompute
# time includes one Python call per node read.
#
# The topologies are graph_bench.py's own builders, loaded from that file, so a name means the same
# graph in both benches; BENCH_TOPO, BENCH_N, BENCH_RES, BENCH_SIZE, BENCH_SEED, BENCH_PLATES_DIR
# and BENCH_NAMED are read exactly as there. This script adds the topology "roto", which defaults
# to BENCH_RES=hd. BENCH_GRAPH_BENCH overrides where graph_bench.py is loaded from (default
# tools/bench/graph_bench.py under the current directory).
#
# Environment (in addition to graph_bench.py's):
#   BENCH_HASH_SAMPLES   timed keystrokes per target, after the warm-up (default 200)
#   BENCH_HASH_WARMUP    untimed keystrokes before timing (default 10)
#   BENCH_ROTO_SHAPES    Beziers in the roto node (default 200)
#   BENCH_ROTO_POINTS    control points per Bezier (default 16)
#   BENCH_ROTO_KEYS      extra keyframes per control point, beyond the creation frame (default 1)
#   BENCH_ROTO_DOWNSTREAM  Grade nodes chained after the Roto node (default 20)
#   BENCH_OUT            JSON-lines file the result is appended to
#
# The Python roto API has no way to create paint strokes, so the heavy roto comp is Beziers only;
# per-point animation stands in for the extra weight strokes would add to the node's content.
#
# The tool needs Effect.getContentHash(), which exists so a script can see that a rehash happened:
# a keystroke that changes no hash is a bug, not a fast result, and is reported as an error.

import json
import math
import os
import sys
import time

# NatronRenderer does not promise what sys.argv[0] is, so the repo-relative path is the fallback.
GRAPH_BENCH = os.environ.get("BENCH_GRAPH_BENCH") or os.path.join(os.getcwd(), "tools", "bench", "graph_bench.py")

SAMPLES = int(os.environ.get("BENCH_HASH_SAMPLES", "200"))
WARMUP = int(os.environ.get("BENCH_HASH_WARMUP", "10"))
ROTO_SHAPES = int(os.environ.get("BENCH_ROTO_SHAPES", "200"))
ROTO_POINTS = int(os.environ.get("BENCH_ROTO_POINTS", "16"))
ROTO_KEYS = int(os.environ.get("BENCH_ROTO_KEYS", "1"))
ROTO_DOWNSTREAM = int(os.environ.get("BENCH_ROTO_DOWNSTREAM", "20"))
OUT = os.environ.get("BENCH_OUT", "")
TOPO = os.environ.get("BENCH_TOPO", "chain")

# The label knob feeds no hash, so editing it costs the setValue path and nothing else.
EXEMPT_KNOB = "userTextArea"

# Knobs a keystroke can edit, per plugin: (param, dimensions, base value). Values only need to
# differ between consecutive edits.
EDITABLE = {
    "Grade": ("multiply", 4, 1.0),
    "Blur": ("size", 2, 3.0),
    "ColorCorrect": ("MasterSaturation", 4, 1.0),
    "Transform": ("translate", 2, 0.25),
    "Merge": ("mix", 1, 0.25),
}


class Recorder(object):
    # Wraps the app global so the nodes a graph_bench builder creates can be listed afterwards.
    def __init__(self, inner):
        self._inner = inner
        self.nodes = []

    def createNode(self, *args):
        n = self._inner.createNode(*args)
        if n is not None:
            self.nodes.append(n)
        return n

    def createReader(self, *args):
        n = self._inner.createReader(*args)
        if n is not None:
            self.nodes.append(n)
        return n

    def __getattr__(self, name):
        return getattr(self._inner, name)


def load_graph_bench(recorder):
    # graph_bench.py runs its benchmark and exits the process at import, so only the part before
    # that entry point is executed: the builders, and the environment parsing they depend on.
    with open(GRAPH_BENCH) as f:
        src = f.read()
    marker = "\ntry:\n    main()"
    cut = src.index(marker)
    ns = {"__name__": "graph_bench", "__file__": GRAPH_BENCH}
    if recorder is not None:
        ns["app"] = recorder
    exec(compile(src[:cut], GRAPH_BENCH, "exec"), ns)
    return ns


def percentile(sorted_vals, q):
    idx = max(0, min(len(sorted_vals) - 1, int(math.ceil(q * len(sorted_vals))) - 1))
    return sorted_vals[idx]


def stats_ms(samples):
    s = sorted(samples)
    return {"med_ms": round(s[len(s) // 2] * 1e3, 4), "p95_ms": round(percentile(s, 0.95) * 1e3, 4),
            "min_ms": round(s[0] * 1e3, 4), "max_ms": round(s[-1] * 1e3, 4)}


def downstream_of(nodes):
    # script name -> names of every node reachable through outputs, built from the inputs
    # because the Python API exposes inputs only.
    consumers = {}
    for e in nodes:
        name = e.getScriptName()
        consumers.setdefault(name, [])
        for i in range(e.getMaxInputCount()):
            src = e.getInput(i)
            if src is not None:
                consumers.setdefault(src.getScriptName(), []).append(name)
    reach = {}
    for e in nodes:
        seen, stack = set(), [e.getScriptName()]
        while stack:
            for nxt in consumers.get(stack.pop(), ()):
                if nxt not in seen:
                    seen.add(nxt)
                    stack.append(nxt)
        reach[e.getScriptName()] = seen
    return reach


def pick_target(nodes, reach):
    # The editable node with the most downstream nodes: the worst-case keystroke of the graph.
    best = None
    for e in nodes:
        if e.getPluginID() not in EDITABLE:
            continue
        if best is None or len(reach[e.getScriptName()]) > len(reach[best.getScriptName()]):
            best = e
    return best


def knob_edit(node):
    name, dims, base = EDITABLE[node.getPluginID()]
    p = node.getParam(name)
    if p is None:
        raise RuntimeError("%s has no param %s" % (node.getPluginID(), name))

    def edit(i):
        v = base + 1e-4 * (i % 1000 + 1)
        p.set(*([v] * dims))
    return "%s.%s" % (node.getPluginID(), name), edit


def control_edit(node):
    p = node.getParam(EXEMPT_KNOB)
    if p is None:
        return None

    def edit(i):
        p.set("k%d" % i)
    return edit


def measure(label, node, edit, control, by_name, reach):
    # Cross-check on the first edit that the node and every downstream node really rehashed.
    names = [node.getScriptName()] + sorted(reach[node.getScriptName()])
    before = [by_name[n].getContentHash() for n in names]
    edit(WARMUP + SAMPLES + 1)
    after = [by_name[n].getContentHash() for n in names]
    rehashed = sum(1 for a, b in zip(before, after) if a != b)
    if rehashed != len(names):
        raise RuntimeError("%s: %d of %d hashes changed after an edit; the keystroke does not "
                           "reach the whole downstream" % (label, rehashed, len(names)))
    if control is not None:
        c_before = node.getContentHash()
        control(0)
        if node.getContentHash() != c_before:
            raise RuntimeError("%s: the control edit changed the node's hash, so it measures "
                               "more than the setValue path" % label)

    hnodes = [by_name[n] for n in names]
    for i in range(WARMUP):
        edit(i)
        for h in hnodes:
            h.getContentHash()
        if control is not None:
            control(i)
    target, ctl, recomp = [], [], []
    clock = time.perf_counter
    for i in range(SAMPLES):
        # Interleaved so slow drift in machine load hits both series alike.
        t0 = clock()
        edit(WARMUP + i)
        t1 = clock()
        target.append(t1 - t0)
        for h in hnodes:
            h.getContentHash()
        recomp.append(clock() - t1)
        if control is not None:
            c0 = clock()
            control(WARMUP + i)
            ctl.append(clock() - c0)
    out = {"name": label, "node": node.getScriptName(), "downstream": len(names) - 1,
           "rehashed": rehashed, "samples": SAMPLES, "setvalue": stats_ms(target), "recompute": stats_ms(recomp)}
    if ctl:
        c_med = sorted(ctl)[len(ctl) // 2]
        out["control"] = stats_ms(ctl)
        net = sorted(max(0.0, t - c_med) for t in target)
        out["net"] = stats_ms(net)
        out["net_p95_under_1ms"] = out["net"]["p95_ms"] < 1.0
    return out


def build_roto(ns):
    make, grade = ns["make"], ns["grade"]
    node = make("Roto")
    ctx = node.getRotoContext()
    if ctx is None:
        raise RuntimeError("Roto node has no Python roto context")
    w, h = ns["SIZES"][ns["RES"]]
    radius = 0.02 * h
    beziers = []
    points = keys = 0
    for s in range(ROTO_SHAPES):
        cx = w * (0.1 + 0.8 * ((s * 37) % 101) / 100.0)
        cy = h * (0.1 + 0.8 * ((s * 53) % 97) / 96.0)
        bez = None
        for k in range(ROTO_POINTS):
            a = 2.0 * math.pi * k / ROTO_POINTS
            x, y = cx + radius * math.cos(a), cy + radius * math.sin(a)
            if bez is None:
                bez = ctx.createBezier(x, y, 1)
                if bez is None:
                    raise RuntimeError("could not create a Bezier")
            else:
                bez.addControlPoint(x, y)
            points += 1
        bez.setCurveFinished(True)
        for key in range(ROTO_KEYS):
            t = 10 * (key + 1)
            for k in range(ROTO_POINTS):
                bez.movePointByIndex(k, t, 0.5 * (key + 1), -0.25 * (key + 1))
                keys += 1
        beziers.append(bez)
    out = node
    for i in range(ROTO_DOWNSTREAM):
        out = grade(out, i)
    return node, beziers, {"roto_shapes": ROTO_SHAPES, "roto_points": points, "roto_keyframes": keys}


def roto_keystrokes(node, beziers):
    first = beziers[0]
    last = beziers[-1]

    def opacity(i):
        first.setOpacity(0.5 + 1e-4 * (i % 1000 + 1), 1)

    def move(i):
        last.movePointByIndex(0, 1, 1e-3 * (1 if i % 2 else -1), 0.0)
    return [("roto.opacity", opacity), ("roto.movePoint", move)]


def main():
    if TOPO == "roto":
        os.environ.setdefault("BENCH_RES", "hd")
    rec = Recorder(app)
    ns = load_graph_bench(rec)

    fmt = ns["PLATE_RES"] if TOPO in ns["READ_TOPOS"] else ns["RES"]
    if fmt == "hd":
        app.getProjectParam("outputFormat").set("HD")
    elif fmt == "uhd":
        app.getProjectParam("outputFormat").set("UHD_4K")
    app.getProjectParam("frameRange").set(1, ns["LAST_FRAME"])

    builders = {"chain": "build_chain", "mixed": "build_mixed", "wide": "build_wide", "comp": "build_comp",
                "readchain": "build_readchain", "footagecomp": "build_footagecomp",
                "iobound": "build_iobound", "rambound": "build_rambound", "deepcomp": "build_deepcomp",
                "ccchain": "build_ccchain", "blurchain": "build_blurchain", "xfchain": "build_xfchain",
                "mergechain": "build_mergechain", "mergesrcchain": "build_mergesrcchain"}
    t0 = time.time()
    extra = {}
    keystrokes = []
    if TOPO == "roto":
        roto_node, beziers, extra = build_roto(ns)
        build_s = time.time() - t0
        nodes = rec.nodes
        by_name = dict((e.getScriptName(), e) for e in nodes)
        reach = downstream_of(nodes)
        for label, edit in roto_keystrokes(roto_node, beziers):
            keystrokes.append((label, roto_node, edit))
    else:
        if TOPO not in builders:
            raise RuntimeError("unknown BENCH_TOPO " + TOPO)
        ns[builders[TOPO]]()
        build_s = time.time() - t0
        nodes = rec.nodes
        by_name = dict((e.getScriptName(), e) for e in nodes)
        reach = downstream_of(nodes)
        target = pick_target(nodes, reach)
        if target is None:
            raise RuntimeError("no node in %s has a known editable knob" % TOPO)
        label, edit = knob_edit(target)
        keystrokes.append((label, target, edit))

    n_nodes = len(nodes)
    print("[hash_bench] built %s nodes=%d in %.2fs" % (TOPO, n_nodes, build_s))
    sys.stdout.flush()

    results = []
    for label, node, edit in keystrokes:
        results.append(measure(label, node, edit, control_edit(node), by_name, reach))
        print("[hash_bench] %s" % json.dumps(results[-1]))
        sys.stdout.flush()

    size = ns["SIZES"].get(fmt)
    result = {"bench": "hash", "topo": TOPO, "n": ns["N"], "nodes": n_nodes, "res": ns["RES"],
              "size": size, "build_s": round(build_s, 4), "samples": SAMPLES, "warmup": WARMUP,
              "keystrokes": results, "cpus": os.cpu_count()}
    result.update(extra)
    line = json.dumps(result)
    print("[hash_bench] RESULT " + line)
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
sys.stdout.flush()
os._exit(0)
