# Render-scaling benchmark for large node graphs. Run inside NatronRenderer:
#
#   BENCH_TOPO=chain BENCH_N=100 BENCH_RES=hd NatronRenderer -b tools/bench/graph_bench.py
#
# Environment:
#   BENCH_TOPO    chain | mixed | wide | comp
#   BENCH_N       approximate number of processing nodes
#   BENCH_RES     tiny (32x32, isolates per-node overhead) | hd (1920x1080) | uhd (3840x2160)
#   BENCH_FRAMES  frames timed one at a time after the warm-up frame (default 3)
#   BENCH_RANGE   frames rendered as one range, to measure frame-parallel throughput (default 0)
#   BENCH_OUT     JSON-lines file the result is appended to
#   BENCH_SEED    seed for the comp topology (default 1)
#   BENCH_SETTINGS  semicolon-separated name=value Natron settings, recorded as "settings"; the shell
#                 scripts pass them to NatronRenderer as --setting arguments (default empty)
#   BENCH_RENDER_STATS  1 saves the project after the timed frames and renders one frame of it with
#                 NatronRenderer --render-stats (app.render cannot enable stats); "Tasks run" and
#                 "Max concurrent tasks" from the -stats.txt file are recorded as tasks_run and
#                 max_concurrent_tasks. Needs the stats-capable CLI path, so it is a separate cold render.
#   BENCH_RENDERER  NatronRenderer binary for that render (default build/release/Renderer/NatronRenderer)
#   BENCH_HOLD    seconds to sleep before rendering, so a sampler can attach (default 0)

import json
import os
import random
import re
import resource
import subprocess
import sys
import time

import NatronEngine

TOPO = os.environ.get("BENCH_TOPO", "chain")
N = int(os.environ.get("BENCH_N", "10"))
RES = os.environ.get("BENCH_RES", "tiny")
FRAMES = int(os.environ.get("BENCH_FRAMES", "3"))
RANGE = int(os.environ.get("BENCH_RANGE", "0"))
OUT = os.environ.get("BENCH_OUT", "")
SEED = int(os.environ.get("BENCH_SEED", "1"))
HOLD = float(os.environ.get("BENCH_HOLD", "0"))
SETTINGS = os.environ.get("BENCH_SETTINGS", "")
WORK = os.environ.get("BENCH_WORK", os.path.join(os.getcwd(), "build", "bench", "work"))
# Explicit names bypass the default unique-name search; BENCH_NAMED=0 measures that path.
NAMED = os.environ.get("BENCH_NAMED", "1") != "0"

LAST_FRAME = 1 + FRAMES + max(RANGE, 0) + 1

counts = {}


def log(msg):
    print("[bench] " + msg)
    sys.stdout.flush()


def make(plugin_id):
    if NAMED:
        name = "n%d" % sum(counts.values())
        props = {"CreateNodeArgsPropNodeInitialName": NatronEngine.StringNodeCreationProperty(name)}
        node = app.createNode(plugin_id, -1, None, props)
    else:
        node = app.createNode(plugin_id)
    if node is None:
        raise RuntimeError("could not create " + plugin_id)
    counts[plugin_id] = counts.get(plugin_id, 0) + 1
    return node


def param(node, name):
    p = node.getParam(name)
    if p is None:
        raise RuntimeError("%s has no param %s" % (node.getPluginID(), name))
    return p


def source(index):
    # Animated so that nothing downstream is treated as frame-invariant and cached once.
    cb = make("net.sf.openfx.CheckerBoardPlugin")
    if RES == "tiny":
        param(cb, "extent").set("Size")
        param(cb, "size").set(32, 32)
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
    param(n, "size").set(3.0, 3.0)
    return n


def merge(a, b):
    n = make("net.sf.openfx.MergePlugin")
    n.connectInput(0, b)
    n.connectInput(1, a)
    param(n, "mix").set(0.5)
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


def build_mixed():
    ops = (grade, blur, transform, colorcorrect)
    out = source(0)
    for i in range(N):
        out = ops[i % len(ops)](out, i)
    return out


def build_wide():
    # Leaves of (source -> grade), reduced by a balanced tree of merges: about 3 nodes per leaf.
    leaves = max(2, (N + 1) // 3)
    level = [grade(source(i), i) for i in range(leaves)]
    while len(level) > 1:
        nxt = []
        for j in range(0, len(level) - 1, 2):
            nxt.append(merge(level[j], level[j + 1]))
        if len(level) % 2:
            nxt.append(level[-1])
        level = nxt
    return level[0]


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


def rusage():
    ru = resource.getrusage(resource.RUSAGE_SELF)
    return ru.ru_utime + ru.ru_stime


def vm(field):
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith(field + ":"):
                return int(line.split()[1]) // 1024
    return -1


def render(writer, first, last):
    c0, t0 = rusage(), time.time()
    app.render(writer, first, last)
    t1, c1 = time.time(), rusage()
    return t1 - t0, c1 - c0


def render_stats(writer):
    # The CLI path (-w) honours --render-stats; the Python app.render does not.
    proj = os.path.join(WORK, "stats_%s_%d.ntp" % (TOPO, N))
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


def main():
    if RES == "hd":
        app.getProjectParam("outputFormat").set("HD")
    elif RES == "uhd":
        app.getProjectParam("outputFormat").set("UHD_4K")
    app.getProjectParam("frameRange").set(1, LAST_FRAME)

    builders = {"chain": build_chain, "mixed": build_mixed, "wide": build_wide, "comp": build_comp}
    t0 = time.time()
    root = builders[TOPO]()
    build_s = time.time() - t0

    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, "%s_%d_%s_####.exr" % (TOPO, N, RES))
    writer = app.createWriter(path)
    writer.connectInput(0, root)
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
    warm_wall, warm_cpu = render(writer, 1, 1)
    walls, cpus = [], []
    for f in range(2, 2 + FRAMES):
        w, c = render(writer, f, f)
        walls.append(w)
        cpus.append(c)
    range_wall = range_cpu = None
    if RANGE > 0:
        first = 2 + FRAMES
        range_wall, range_cpu = render(writer, first, first + RANGE - 1)

    stats_fields = render_stats(writer) if os.environ.get("BENCH_RENDER_STATS") == "1" else {}
    med = sorted(walls)[len(walls) // 2] if walls else None
    result = {
        "topo": TOPO,
        "n": N,
        "nodes": n_nodes,
        "res": RES,
        "named": NAMED,
        "build_s": round(build_s, 4),
        "warm_wall_s": round(warm_wall, 4),
        "warm_cpu_s": round(warm_cpu, 4),
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
    }
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
