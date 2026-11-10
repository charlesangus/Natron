"""Checks gen_graph.py inside NatronRenderer:

    NatronRenderer -t tools/bench/gui/test_gen_graph.py < /dev/null

For each topology at N=300 it builds the graph twice with one seed and once with another, and
checks the processing-node, edge and expression counts against the graph Natron actually holds,
that one seed gives identical summaries (including the hash of names, inputs and expressions), and
that another seed gives a different hash. Prints PASS/FAIL lines and exits non-zero on a failure.
"""
import os
import sys
import traceback

import gen_graph

N = int(os.environ.get("GEN_GRAPH_TEST_N", "300"))
SEED = 7

failures = []


def check(ok, label):
    print(("PASS " if ok else "FAIL ") + label)
    sys.stdout.flush()
    if not ok:
        failures.append(label)
    return ok


def walk(collection, prefix=""):
    """(full name, effect) for every node in collection and in the groups under it."""
    for child in collection.getChildren():
        name = prefix + child.getScriptName()
        yield name, child
        if child.getPluginID() == gen_graph.GROUP:
            for item in walk(child, name + "."):
                yield item


def actual_counts():
    nodes = edges = 0
    for _name, node in walk(app):
        if node.getPluginID() in gen_graph.PROCESSING:
            nodes += 1
        for i in range(node.getMaxInputCount()):
            if node.getInput(i) is not None:
                edges += 1
    return nodes, edges


def expressions_present(summary):
    present = 0
    for full_name, knob, expr in summary["expression_list"]:
        node = app.getNode(full_name)
        if node is None:
            continue
        got = node.getParam(knob).getExpression(0)
        text = got[0] if isinstance(got, (tuple, list)) else got
        if text == expr:
            present += 1
    return present


def fresh():
    if not app.resetProject():
        raise RuntimeError("resetProject failed")


def key(summary):
    return dict((k, summary[k]) for k in gen_graph.SUMMARY_KEYS)


def run():
    for topology in gen_graph.TOPOLOGIES:
        fresh()
        first = gen_graph.build(app, topology, N, SEED)
        label = "%s n=%d" % (topology, N)
        print("NOTE %s %s" % (label, key(first)))
        check(first["nodes"] == N, "%s: summary counts %d processing nodes" % (label, first["nodes"]))
        nodes, edges = actual_counts()
        check(nodes == N, "%s: the project holds %d processing nodes (got %d)" % (label, N, nodes))
        check(edges == first["edges"], "%s: the project holds the %d edges reported (got %d)" % (label, first["edges"], edges))
        slots = first["expr_slots"]
        frac = first["expressions"] / float(slots) if slots else 0.0
        check(0.1 <= frac <= 0.3, "%s: %d of %d knob slots carry expressions (%.2f)" % (label, first["expressions"], slots, frac))
        present = expressions_present(first)
        check(present == first["expressions"], "%s: all %d expressions read back (got %d)" % (label, first["expressions"], present))
        check(first["labels"] == (N + 9) // 10, "%s: every tenth node has a label (%d)" % (label, first["labels"]))
        check(first["backdrops"] >= 1, "%s: backdrops were added (%d)" % (label, first["backdrops"]))
        check(first["groups"] == 3, "%s: three groups at this size (%d)" % (label, first["groups"]))

        fresh()
        second = gen_graph.build(app, topology, N, SEED)
        check(key(first) == key(second), "%s: the same seed gives an identical summary and hash" % label)

        fresh()
        other = gen_graph.build(app, topology, N, SEED + 1)
        check(other["hash"] != first["hash"], "%s: another seed gives another hash" % label)
    fresh()


try:
    run()
except Exception:
    check(False, "error: " + traceback.format_exc())
print("SUMMARY %d failure(s)" % len(failures))
sys.stdout.flush()
os._exit(1 if failures else 0)
