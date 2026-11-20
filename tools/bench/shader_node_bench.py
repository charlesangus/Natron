#!/usr/bin/env python3
"""Cost of a 50-node port network against the fixed bar, printing PASS or FAIL per bar.

  python3 tools/bench/shader_node_bench.py [--build build/release] [--render-dir DIR] [--no-graph-bench]

Runs from the checkout root, inside natron-dev, after a build. Two parts:

  1. The Tests binary's ShaderNodeCost and registration gtests, which build the network of
     5 large materials, 10 BSDFs, 5 two-output textures and 30 maps in a Group, twice: knob-first
     (about 10 ports per node, every other attribute a knob) and with every attribute a port. They
     print SHADER_BENCH lines: create, save, open, a knob edit plus re-authoring the network's
     UsdShade layer through SceneNodeAuthoring::openLayerStage, and in-process comp edits (a Grade
     knob change and a reconnection per round) before and after the network is added.
  2. graph_bench.py's gxg session (BENCH_SESSION=bc) in NatronRenderer without a network and with
     each variant (BENCH_SHADER_NETWORK), comparing the re-render after each knob change.

Bars: open < 2 s, save < 2 s, a knob edit re-authors in < 100 ms, comp edits
no slower with the network loaded (within --tolerance, default 10 %, for timing noise). Only the
knob-first network has to meet them; the every-attribute-a-port network is printed for
comparison. Exits 1 if a bar fails.

  --tests BIN       the Tests binary (default <build>/Tests/Tests)
  --renderer BIN    NatronRenderer (default <build>/Renderer/NatronRenderer)
  --render-dir DIR  where graph_bench writes its frames (default build/bench/shader-node-render)
  --json FILE       appends every measured record to FILE as JSON lines
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys

BARS_MS = {"open_ms": 2000.0, "save_ms": 2000.0, "knob_edit_ms": 100.0}
BAR_LABELS = {"open_ms": "open", "save_ms": "save", "knob_edit_ms": "knob edit re-authors"}
VARIANTS = ("knob-first", "every-attribute-a-port")


def run(cmd, env=None):
    print("$ " + " ".join(cmd))
    sys.stdout.flush()
    proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return proc.returncode, proc.stdout


def gtest_records(tests):
    status, out = run([tests, "--gtest_filter=ShaderNodeCostTest.*:ShaderNodeInputsTest.RegistrationAndSpecCosts"])
    records = [json.loads(m.group(1)) for m in re.finditer(r"^SHADER_BENCH (\{.*\})$", out, re.M)]
    if status != 0:
        sys.stdout.write(out[-4000:])
        raise SystemExit("the gtests failed (exit %d)" % status)
    return records


def graph_bench_record(renderer, render_dir, network):
    env = dict(os.environ)
    env.update({"NATRON_GUI_TEST_NODES": "1", "BENCH_TOPO": "gxg", "BENCH_NODE": "blur", "BENCH_SESSION": "bc",
                "BENCH_RERENDERS": "8", "BENCH_RENDER_DIR": render_dir, "BENCH_SHADER_NETWORK": network,
                "BENCH_LABEL": network or "none"})
    cmd = [renderer, "-b", "tools/bench/graph_bench.py"]
    if shutil.which("xvfb-run"):
        cmd = ["xvfb-run", "--auto-servernum"] + cmd
    status, out = run(cmd, env)
    found = re.findall(r"RESULT (\{.*\})", out)
    if status != 0 or not found:
        sys.stdout.write(out[-4000:])
        raise SystemExit("graph_bench failed (exit %d) for network %r" % (status, network))
    record = json.loads(found[-1])
    rerenders = [m["wall_s"] for m in record["renders"] if m["label"] == "b"]
    return {"test": "graph_bench", "network": network or None, "setup_s": record["setup_s"],
            "rerender_median_s": statistics.median(rerenders), "rerenders": rerenders}


def verdict(ok):
    return "PASS" if ok else "FAIL"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default=os.path.join("build", "release"))
    parser.add_argument("--tests")
    parser.add_argument("--renderer")
    parser.add_argument("--render-dir", default=os.path.abspath(os.path.join("build", "bench", "shader-node-render")))
    parser.add_argument("--no-graph-bench", action="store_true")
    parser.add_argument("--tolerance", type=float, default=0.10)
    parser.add_argument("--json")
    args = parser.parse_args()
    tests = args.tests or os.path.join(args.build, "Tests", "Tests")
    renderer = args.renderer or os.path.join(args.build, "Renderer", "NatronRenderer")

    records = gtest_records(tests)
    networks = {r["variant"]: r for r in records if r.get("test") == "network"}
    missing = [v for v in VARIANTS if v not in networks]
    if missing:
        raise SystemExit("no SHADER_BENCH record for %s" % ", ".join(missing))

    graph = []
    if not args.no_graph_bench:
        os.makedirs(args.render_dir, exist_ok=True)
        for network in ("",) + VARIANTS:
            graph.append(graph_bench_record(renderer, args.render_dir, network))
        records.extend(graph)

    failed = False
    print()
    print("%-24s %-26s %12s %12s  %s" % ("variant", "bar", "measured", "limit", "result"))
    for variant in VARIANTS:
        r = networks[variant]
        gated = variant == "knob-first"
        for key, limit in BARS_MS.items():
            ok = r[key] < limit
            failed = failed or (gated and not ok)
            print("%-24s %-26s %10.1fms %10.0fms  %s%s" % (variant, BAR_LABELS[key], r[key], limit, verdict(ok), "" if gated else " (comparison)"))
        limit = r["comp_edit_without_ms"] * (1.0 + args.tolerance)
        ok = r["comp_edit_with_ms"] <= limit
        failed = failed or (gated and not ok)
        print("%-24s %-26s %10.1fms %10.1fms  %s%s" % (variant, "comp edits (in process)", r["comp_edit_with_ms"], limit, verdict(ok), "" if gated else " (comparison)"))
    if graph:
        base = graph[0]["rerender_median_s"]
        for g in graph[1:]:
            limit = base * (1.0 + args.tolerance)
            ok = g["rerender_median_s"] <= limit
            gated = g["network"] == "knob-first"
            failed = failed or (gated and not ok)
            print("%-24s %-26s %11.4fs %11.4fs  %s%s" % (g["network"], "comp edits (graph_bench)", g["rerender_median_s"], limit, verdict(ok), "" if gated else " (comparison)"))
    for r in records:
        if r.get("test") == "registration":
            print("registration: %.1f us per plug-in entry, %.1f us per port edit (%d kinds)" % (r["register_entry_us"], r["port_edit_us"], r["kinds"]))
    for variant in VARIANTS:
        r = networks[variant]
        print("%s: %d nodes, %d edges, %d inputs, %d knobs, create %.1f ms, knob edit max %.1f ms"
              % (variant, r["nodes"], r["edges"], r["inputs"], r["knobs"], r["create_ms"], r["knob_edit_max_ms"]))

    if args.json:
        with open(args.json, "a") as f:
            for r in records:
                f.write(json.dumps(r) + "\n")
    print()
    print("overall: %s" % verdict(not failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
