"""Seeded large-graph generator for the GUI benchmark. Runs inside Natron (GUI or NatronRenderer):

    import gen_graph
    summary = gen_graph.build(app, "mixed", 1000, seed=1)

Topologies, each with n processing nodes (Grade, Blur, Transform, ColorCorrect, Merge; sources,
groups, their Input/Output nodes and backdrops are extra):
    chain   one source feeding n nodes in series
    wide    one source fanning out into n nodes
    comp    a chain of diamonds: a stream split into two branches and merged back, the shape whose
            upstream walks went exponential before they were memoised
    mixed   comp with side branches of 1-4 nodes fanning off the spine

About expr_frac of the numeric knob slots get an expression referencing an earlier node's knob in
the same group, or the frame. Every tenth node gets a 3-5 line HTML label, a backdrop wraps every
50 root nodes, and about 5% of the nodes sit inside 3 or 4 groups on the spine.

Nodes get explicit names, so setup never pays the unique-name search. The same arguments always
give the same names, connections and expressions; the summary's "hash" covers all three.
"""
import hashlib
import json
import random

import NatronEngine

GRADE = "net.sf.openfx.GradePlugin"
BLUR = "net.sf.cimg.CImgBlur"
TRANSFORM = "net.sf.openfx.TransformPlugin"
COLORCORRECT = "net.sf.openfx.ColorCorrectPlugin"
MERGE = "Merge"
SOURCE = "net.sf.openfx.CheckerBoardPlugin"
GROUP = "fr.inria.built-in.Group"
GROUP_INPUT = "fr.inria.built-in.Input"
GROUP_OUTPUT = "fr.inria.built-in.Output"
BACKDROP = "fr.inria.built-in.BackDrop"

KIND_PREFIX = {GRADE: "Grade", BLUR: "Blur", TRANSFORM: "Transform", COLORCORRECT: "ColorCorrect", MERGE: "Merge"}
PROCESSING = frozenset(KIND_PREFIX)
FILTERS = (GRADE, BLUR, TRANSFORM, COLORCORRECT)

# Numeric knobs that may get an expression on their first dimension. Knobs a build lacks are
# skipped, consistently, so counts stay deterministic for one build.
EXPR_KNOBS = {
    GRADE: ("multiply", "gamma", "offset"),
    BLUR: ("size",),
    TRANSFORM: ("translate", "rotate", "scale"),
    COLORCORRECT: ("MasterSaturation", "MasterGain"),
    MERGE: ("mix",),
}
LABEL_KNOB = "userTextArea"

TOPOLOGIES = ("chain", "wide", "comp", "mixed")

COL_W = 900.0
ROW_H = 90.0
BRANCH_DX = 130.0
ROWS_PER_COLUMN = 60
BACKDROP_EVERY = 50
BACKDROP_PAD = 40.0
GROUP_FRACTION = 0.05


class _Builder(object):
    def __init__(self, app, topology, n, seed, expr_frac, labels, backdrops, groups):
        self.app = app
        self.topology = topology
        self.n = n
        self.rng = random.Random(seed)
        self.expr_frac = expr_frac
        self.labels = labels
        self.backdrops = backdrops
        self.groups = groups
        self.serial = 0
        self.made = 0
        self.edges = 0
        self.expr_slots = 0
        self.expressions = []
        self.label_nodes = []
        self.root_nodes = []
        self.group_names = []
        self.backdrop_names = []
        # (group name or "", name) -> plugin ID, in creation order, per collection.
        self.by_collection = {}
        self.knobs_present = {}
        self.row = 0
        self.col = 0

    # ---- creation ------------------------------------------------------------------------------

    def _name(self, prefix):
        self.serial += 1
        return "%s%d" % (prefix, self.serial)

    def _create(self, plugin_id, prefix, group=None, group_name=""):
        name = self._name(prefix)
        props = {"CreateNodeArgsPropNodeInitialName": NatronEngine.StringNodeCreationProperty(name)}
        node = self.app.createNode(plugin_id, -1, group, props)
        if node is None:
            raise RuntimeError("could not create %s (%s)" % (plugin_id, name))
        if not group_name:
            self.root_nodes.append(name)
        return node

    def _connect(self, node, index, source):
        if not node.connectInput(index, source):
            raise RuntimeError("could not connect %s input %d to %s" %
                               (node.getScriptName(), index, source.getScriptName()))
        self.edges += 1

    def _place(self, node, dx=0.0):
        node.setPosition(self.col * COL_W + dx, self.row * ROW_H)

    def _advance_row(self):
        self.row += 1
        if self.row >= ROWS_PER_COLUMN:
            self.row = 0
            self.col += 1

    def _processing(self, plugin_id, inputs, group=None, group_name="", dx=0.0):
        node = self._create(plugin_id, KIND_PREFIX[plugin_id], group, group_name)
        for index, source in inputs:
            self._connect(node, index, source)
        name = node.getScriptName()
        if group is None:
            self._place(node, dx)
        index = self.made
        self.made += 1
        self._add_expressions(node, plugin_id, name, group_name)
        if self.labels and index % 10 == 0:
            self._add_label(node, name, group_name, index)
        self.by_collection.setdefault(group_name, []).append((name, plugin_id))
        return node

    def _filter(self, source, group=None, group_name="", dx=0.0):
        return self._processing(self.rng.choice(FILTERS), [(0, source)], group, group_name, dx)

    def _merge(self, b, a, group=None, group_name="", dx=0.0):
        return self._processing(MERGE, [(0, b), (1, a)], group, group_name, dx)

    # ---- decoration ----------------------------------------------------------------------------

    def _present_knobs(self, node, plugin_id):
        if plugin_id not in self.knobs_present:
            self.knobs_present[plugin_id] = tuple(k for k in EXPR_KNOBS[plugin_id] if node.getParam(k) is not None)
        return self.knobs_present[plugin_id]

    def _add_expressions(self, node, plugin_id, name, group_name):
        earlier = self.by_collection.get(group_name, [])
        for knob in self._present_knobs(node, plugin_id):
            self.expr_slots += 1
            if self.rng.random() >= self.expr_frac:
                continue
            refs = [(n, self.knobs_present[p]) for n, p in earlier[-200:] if self.knobs_present.get(p)]
            if refs and self.rng.random() < 0.75:
                ref_name, ref_knobs = self.rng.choice(refs)
                expr = "%s.%s.getValue(0) * 0.5" % (ref_name, self.rng.choice(ref_knobs))
            else:
                expr = "1 + 0.01 * frame"
            if not node.getParam(knob).setExpression(expr, False, 0):
                raise RuntimeError("could not set %s.%s = %s" % (name, knob, expr))
            self.expressions.append((self._full(group_name, name), knob, expr))

    def _add_label(self, node, name, group_name, index):
        param = node.getParam(LABEL_KNOB)
        if param is None:
            return
        lines = ["<b>%s</b> <i>pass %d</i>" % (name, index),
                 "<font color=\"#c0a060\">shot sq%03d_sh%04d</font>" % (index // 100, index),
                 "note: keep the %s from plate v%d" % (self.rng.choice(("edge", "grain", "glow", "key")), self.rng.randrange(1, 20))]
        for _ in range(self.rng.randrange(0, 3)):
            lines.append("<u>todo</u> %s" % self.rng.choice(("despill", "re-time", "match blacks", "add flare")))
        param.set("<br>".join(lines))
        self.label_nodes.append(self._full(group_name, name))

    @staticmethod
    def _full(group_name, name):
        return group_name + "." + name if group_name else name

    # ---- groups --------------------------------------------------------------------------------

    def _group_plan(self):
        if not self.groups or self.n < 40:
            return []
        count = 3 if self.n < 1000 else 4
        inner = max(1, int(round(self.n * GROUP_FRACTION / count)))
        # Node counts at which a group replaces the next spine step, spread evenly.
        spacing = max(1, (self.n - inner * count) // (count + 1))
        return [((i + 1) * spacing, inner) for i in range(count)]

    def _group(self, source, inner):
        group = self._create(GROUP, "Group")
        gname = group.getScriptName()
        self.group_names.append(gname)
        self._connect(group, 0, source)
        self._place(group)
        children = list(group.getChildren())
        g_in = next((c for c in children if c.getPluginID() == GROUP_INPUT), None)
        g_out = next((c for c in children if c.getPluginID() == GROUP_OUTPUT), None)
        if g_in is None or g_out is None:
            raise RuntimeError("group %s has no Input or Output node" % gname)
        stream = g_in
        for i in range(inner):
            stream = self._filter(stream, group, gname)
            stream.setPosition(0.0, (i + 1) * ROW_H)
        self._connect(g_out, 0, stream)
        g_out.setPosition(0.0, (inner + 1) * ROW_H)
        return group, inner

    # ---- topologies ----------------------------------------------------------------------------

    def _source(self):
        node = self._create(SOURCE, "Source")
        self._place(node)
        self._advance_row()
        return node

    def build(self):
        builder = getattr(self, "_build_" + self.topology, None)
        if builder is None:
            raise ValueError("unknown topology %r (one of %s)" % (self.topology, ", ".join(TOPOLOGIES)))
        builder()
        if self.backdrops:
            self._add_backdrops()

    def _spine(self, step):
        """Runs step(stream) until n nodes exist, swapping in the planned groups."""
        plan = self._group_plan()
        stream = self._source()
        while self.made < self.n:
            if plan and self.made >= plan[0][0] and self.made + plan[0][1] <= self.n:
                stream, _ = self._group(stream, plan.pop(0)[1])
                self._advance_row()
                continue
            stream = step(stream)
        return stream

    def _build_chain(self):
        def step(stream):
            node = self._filter(stream)
            self._advance_row()
            return node
        self._spine(step)

    def _build_wide(self):
        plan = self._group_plan()
        source = self._source()
        side = max(1, int(self.n ** 0.5))
        top = self.row
        slot = 0
        while self.made < self.n:
            if plan and self.made >= plan[0][0] and self.made + plan[0][1] <= self.n:
                node, _ = self._group(source, plan.pop(0)[1])
            else:
                node = self._filter(source)
            node.setPosition((slot % side) * 160.0, (top + slot // side) * ROW_H)
            slot += 1

    def _diamond(self, stream):
        remaining = self.n - self.made
        if remaining < 3:
            node = self._filter(stream)
            self._advance_row()
            return node
        self._advance_row()
        left = self._filter(stream, dx=-BRANCH_DX)
        right = self._filter(stream, dx=BRANCH_DX)
        self._advance_row()
        merged = self._merge(left, right)
        self._advance_row()
        return merged

    def _build_comp(self):
        self._spine(self._diamond)

    def _build_mixed(self):
        def step(stream):
            out = self._diamond(stream)
            if self.made < self.n and self.rng.random() < 0.3:
                branch = out
                for k in range(min(self.rng.randrange(1, 5), self.n - self.made)):
                    branch = self._filter(branch, dx=2 * BRANCH_DX + k * BRANCH_DX)
            return out
        self._spine(step)

    # ---- backdrops -----------------------------------------------------------------------------

    def _add_backdrops(self):
        members = [n for n in self.root_nodes if not n.startswith("Source")]
        for start in range(0, len(members), BACKDROP_EVERY):
            boxes = []
            for name in members[start:start + BACKDROP_EVERY]:
                node = self.app.getNode(name)
                x, y = node.getPosition()
                w, h = node.getSize()
                boxes.append((x, y, x + w, y + h))
            if not boxes:
                continue
            x0 = min(b[0] for b in boxes) - BACKDROP_PAD
            y0 = min(b[1] for b in boxes) - BACKDROP_PAD
            x1 = max(b[2] for b in boxes) + BACKDROP_PAD
            y1 = max(b[3] for b in boxes) + BACKDROP_PAD
            backdrop = self._create(BACKDROP, "Backdrop")
            backdrop.setPosition(x0, y0)
            backdrop.setSize(x1 - x0, y1 - y0)
            self.backdrop_names.append(backdrop.getScriptName())

    # ---- summary -------------------------------------------------------------------------------

    def summary(self):
        digest = hashlib.sha1()
        for name in self.root_nodes:
            self._hash_node(digest, self.app.getNode(name), name)
        for gname in self.group_names:
            group = self.app.getNode(gname)
            for child in sorted(group.getChildren(), key=lambda c: c.getScriptName()):
                self._hash_node(digest, child, gname + "." + child.getScriptName())
        for record in self.expressions:
            digest.update(json.dumps(record).encode("utf-8"))
        return {
            "topology": self.topology,
            "n": self.n,
            "nodes": self.made,
            "edges": self.edges,
            "expressions": len(self.expressions),
            "expr_slots": self.expr_slots,
            "labels": len(self.label_nodes),
            "backdrops": len(self.backdrop_names),
            "groups": len(self.group_names),
            "hash": digest.hexdigest(),
            "root_nodes": list(self.root_nodes),
            "label_nodes": list(self.label_nodes),
            "group_names": list(self.group_names),
            "expression_list": [list(e) for e in self.expressions],
        }

    @staticmethod
    def _hash_node(digest, node, full_name):
        inputs = []
        for i in range(node.getMaxInputCount()):
            inp = node.getInput(i)
            inputs.append(inp.getScriptName() if inp is not None else None)
        digest.update(json.dumps([full_name, node.getPluginID(), inputs]).encode("utf-8"))


# Keys that describe the graph; the rest of the summary lists names for the benchmark to use.
SUMMARY_KEYS = ("topology", "n", "nodes", "edges", "expressions", "expr_slots", "labels", "backdrops", "groups", "hash")


def build(app, topology, n, seed, expr_frac=0.2, labels=True, backdrops=True, groups=True):
    builder = _Builder(app, topology, int(n), seed, expr_frac, labels, backdrops, groups)
    builder.build()
    return builder.summary()
