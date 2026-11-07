"""Picks the rows heavy_scene_bench.py acts on from a scene's composed stage.

  heavy_scene_rows.py <scene> [--load all|none] [--boxes 10000] [--out rows.json]

Prints one JSON object. It runs in its own process, under the AppImage's natron-python or any python
with pxr: opening the stage inside Natron would leave its layers in the registry and hand the
viewer's own open an already-loaded scene.

--load is the load state the bench will run in, because that decides which prims exist. Prims are
those the viewer's rows see: active, defined, not abstract, unloaded included. Instance proxies are
never candidates, since neither mechanism can act on one: they have no spec path to author on and no
prim of their own in Hydra. An instance counts its prototype's prims and gprims. An unloaded payload
prim counts as holding geometry.

  root           the default prim, otherwise the root prim holding the most gprims
  branch         the deepest prim holding more than half of the root's gprims (prune_restore,
                 noedit, reveal_cancel), so a state change costs what most of the scene costs
  widest_row     the prim with the most children that hold geometry
  sibling_row    the child of widest_row with the largest world bound (sibling_state, sibling_shot):
                 it has the most siblings the scene offers, and its change is the likeliest to show
                 in a screenshot
  reveal_branch  root
  box_depth      the shallowest path depth with at least --boxes geometry-holding prims, otherwise
                 the depth with the most of them

The payload rows come from a second, LoadNone composition, so they are the same in either load state:

  load_building  the middle payload prim in path order
  load_block     the prim other than root with the most payload prims below it, otherwise root
  chain_block    load_block
  chain_names    up to six prim names most repeated below chain_block with it loaded, among prims
                 that hold geometry; what the chain's pattern nodes match

Ties go to the first path in sort order. A row that does not exist is null, and the bench records the
scenarios that need it as skipped.
"""
import argparse
import collections
import json
import os
import sys
import time

from pxr import Ar, Sdf, Usd, UsdGeom

ROW_PREDICATE = Usd.PrimIsActive & Usd.PrimIsDefined & ~Usd.PrimIsAbstract
BOUND_SAMPLE = 4096
CHAIN_NAMES = 6


class Tally(object):
    """Prims and gprims of a stage's rows, with each instance expanded through its prototype."""

    def __init__(self, stage):
        self.stage = stage
        self.prototypes = {}
        self.rows = {}
        self.children = collections.defaultdict(list)
        self.instances = 0
        for prim in Usd.PrimRange.Stage(stage, ROW_PREDICATE):
            self.rows[prim.GetPath()] = list(self._own(prim))
            self.instances += 1 if prim.IsInstance() else 0
            self.children[prim.GetPath().GetParentPath()].append(prim.GetPath())
        for path in sorted(self.rows, key=lambda p: p.pathElementCount, reverse=True):
            parent = path.GetParentPath()
            if parent in self.rows:
                self.rows[parent][0] += self.rows[path][0]
                self.rows[parent][1] += self.rows[path][1]

    def _own(self, prim):
        prims, gprims = 1, 0
        if prim.IsA(UsdGeom.Gprim):
            gprims = 1
        elif prim.HasAuthoredPayloads() and not prim.IsLoaded():
            gprims = 1
        if prim.IsInstance():
            inner = self._prototype(prim.GetPrototype())
            prims += inner[0]
            gprims += inner[1]
        return prims, gprims

    def _prototype(self, prototype):
        key = prototype.GetPath()
        if key not in self.prototypes:
            total = [0, 0]
            for prim in Usd.PrimRange(prototype, ROW_PREDICATE):
                if prim.GetPath() == key:
                    continue
                own = self._own(prim)
                total[0] += own[0]
                total[1] += own[1]
            self.prototypes[key] = total
        return self.prototypes[key]

    def prims(self, path):
        return self.rows[path][0]

    def gprims(self, path):
        return self.rows[path][1]

    def geometry_children(self, path):
        return sorted(c for c in self.children.get(path, []) if self.gprims(c) > 0)


def pick_root(stage, tally):
    default = stage.GetDefaultPrim()
    if default and default.GetPath() in tally.rows:
        return default.GetPath()
    candidates = sorted(tally.children.get(Sdf.Path.absoluteRootPath, []))
    if not candidates:
        return None
    return max(candidates, key=lambda p: (tally.gprims(p), tally.prims(p)))


def pick_branch(tally, root):
    current = root
    half = tally.gprims(root) / 2.0
    while True:
        holding = [c for c in tally.geometry_children(current) if tally.gprims(c) > half]
        if not holding:
            return current
        current = holding[0]


def pick_widest(tally):
    best = None
    best_key = None
    for path in sorted(tally.rows):
        count = len(tally.geometry_children(path))
        if count < 2:
            continue
        key = (count, tally.gprims(path))
        if best is None or key > best_key:
            best, best_key = path, key
    return best


def pick_sibling(stage, tally, widest):
    children = tally.geometry_children(widest)
    step = max(1, len(children) // BOUND_SAMPLE)
    cache = UsdGeom.BBoxCache(Usd.TimeCode.Default(), [UsdGeom.Tokens.default_, UsdGeom.Tokens.proxy], True)
    best = children[0]
    best_size = -1.0
    for path in children[::step]:
        box = cache.ComputeWorldBound(stage.GetPrimAtPath(path)).ComputeAlignedRange()
        size = 0.0 if box.IsEmpty() else box.GetSize().GetLength()
        if size > best_size:
            best, best_size = path, size
    return best


def pick_box_depth(tally, boxes):
    at_depth = collections.Counter(p.pathElementCount for p in tally.rows if tally.gprims(p) > 0)
    if not at_depth:
        return None, 0
    enough = sorted(d for d, n in at_depth.items() if n >= boxes)
    depth = enough[0] if enough else min(at_depth, key=lambda d: (-at_depth[d], d))
    return depth, at_depth[depth]


def payload_assets(prim):
    assets = set()
    for spec in prim.GetPrimStack():
        for payload in spec.payloadList.GetAddedOrExplicitItems():
            if not payload.assetPath:
                continue
            if spec.layer.anonymous:
                continue
            anchored = Sdf.ComputeAssetPathRelativeToLayer(spec.layer, payload.assetPath)
            resolved = Ar.GetResolver().Resolve(anchored)
            assets.add(str(resolved) or anchored)
    return assets


def pick_payload_rows(scene, root):
    """The load rows, from an unloaded composition, which also shows which files are payloads."""
    empty = {"payload_rows": 0, "load_building": None, "load_block": None, "chain_block": None,
             "chain_names": [], "payload_assets": []}
    if root is None:
        return empty
    stage = Usd.Stage.Open(scene, load=Usd.Stage.LoadNone)
    rows = sorted(p.GetPath() for p in Usd.PrimRange.Stage(stage, ROW_PREDICATE) if p.HasAuthoredPayloads())
    if not rows:
        return empty
    assets = set()
    below = collections.Counter()
    for path in rows:
        assets |= payload_assets(stage.GetPrimAtPath(path))
        for ancestor in path.GetAncestorsRange():
            if ancestor != path and ancestor != root and ancestor != Sdf.Path.absoluteRootPath:
                below[ancestor] += 1
    block = root
    if below:
        block = min(below, key=lambda p: (-below[p], p))
    stage.Load(block, Usd.LoadWithDescendants)
    tally = Tally(stage)
    names = collections.Counter(p.name for p in tally.rows if p != block and p.HasPrefix(block) and tally.gprims(p) > 0)
    chosen = sorted(names, key=lambda n: (-names[n], n))[:CHAIN_NAMES]
    return {"payload_rows": len(rows), "load_building": str(rows[len(rows) // 2]), "load_block": str(block),
            "chain_block": str(block), "chain_names": chosen, "payload_assets": sorted(assets)}


def pick(scene, load, boxes):
    started = time.time()
    stage = Usd.Stage.Open(scene, load=Usd.Stage.LoadAll if load == "all" else Usd.Stage.LoadNone)
    if stage is None:
        raise SystemExit("cannot open " + scene)
    tally = Tally(stage)
    root = pick_root(stage, tally)
    rows = {"scene": os.path.abspath(scene), "load": load, "root": str(root) if root else None,
            "prims": 0, "gprims": 0, "instances": tally.instances, "prototypes": len(tally.prototypes),
            "branch": None, "branch_gprims": 0, "widest_row": None, "widest_children": 0, "sibling_row": None,
            "sibling_kind": None, "reveal_branch": None, "box_depth": None, "box_depth_rows": 0}
    if root is not None and tally.gprims(root) > 0:
        rows["prims"] = tally.prims(root)
        rows["gprims"] = tally.gprims(root)
        branch = pick_branch(tally, root)
        rows["branch"] = str(branch)
        rows["branch_gprims"] = tally.gprims(branch)
        rows["reveal_branch"] = str(root)
        rows["box_depth"], rows["box_depth_rows"] = pick_box_depth(tally, boxes)
        widest = pick_widest(tally)
        if widest is not None:
            sibling = pick_sibling(stage, tally, widest)
            rows["widest_row"] = str(widest)
            rows["widest_children"] = len(tally.geometry_children(widest))
            rows["sibling_row"] = str(sibling)
            rows["sibling_kind"] = Usd.ModelAPI(stage.GetPrimAtPath(sibling)).GetKind() or None
    rows.update(pick_payload_rows(scene, root))
    rows["pick_s"] = round(time.time() - started, 1)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("scene")
    parser.add_argument("--load", choices=("all", "none"), default="all")
    parser.add_argument("--boxes", type=int, default=10000)
    parser.add_argument("--out")
    args = parser.parse_args()
    rows = pick(args.scene, args.load, args.boxes)
    text = json.dumps(rows, indent=1, sort_keys=True)
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        temporary = args.out + ".tmp"
        with open(temporary, "w") as f:
            f.write(text + "\n")
        os.replace(temporary, args.out)
    sys.stdout.write(text + "\n")


if __name__ == "__main__":
    main()
