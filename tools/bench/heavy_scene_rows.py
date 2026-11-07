"""Picks the rows heavy_scene_bench.py acts on from a scene's composed stage.

  heavy_scene_rows.py <scene> [--load all|none] [--out rows.json]

Prints one JSON object. It runs in its own process, under the AppImage's natron-python or any python
with pxr: opening the stage inside Natron would leave its layers in the registry and hand the
viewer's own open an already-loaded scene.

--load is the load state the counts are taken in (default none, which is how the viewer opens a
scene). Prims are those the viewer's rows see: active, defined, not abstract, unloaded included. An
instance counts its prototype's prims and gprims. An unloaded payload prim counts as holding geometry.

  root           the default prim, otherwise the root prim holding the most gprims

The payload rows come from a second, LoadNone composition, so they are the same in either load state:

  payload_rows   how many prims carry a payload
  load_building  the middle payload prim in path order
  load_block     the prim other than root with the most payload prims below it, otherwise root
  twirl_row      the payload prim after load_building, so a twirl after a load still has work to do,
                 otherwise load_building
  payload_assets       every file those prims name
  payload_row_assets   the files each payload prim names

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


def pick_root(stage, tally):
    default = stage.GetDefaultPrim()
    if default and default.GetPath() in tally.rows:
        return default.GetPath()
    candidates = sorted(tally.children.get(Sdf.Path.absoluteRootPath, []))
    if not candidates:
        return None
    return max(candidates, key=lambda p: (tally.gprims(p), tally.prims(p)))


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
    empty = {"payload_rows": 0, "load_building": None, "load_block": None, "twirl_row": None,
             "payload_assets": [], "payload_row_assets": {}}
    if root is None:
        return empty
    stage = Usd.Stage.Open(scene, load=Usd.Stage.LoadNone)
    rows = sorted(p.GetPath() for p in Usd.PrimRange.Stage(stage, ROW_PREDICATE) if p.HasAuthoredPayloads())
    if not rows:
        return empty
    row_assets = {}
    below = collections.Counter()
    for path in rows:
        row_assets[str(path)] = sorted(payload_assets(stage.GetPrimAtPath(path)))
        for ancestor in path.GetAncestorsRange():
            if ancestor != path and ancestor != root and ancestor != Sdf.Path.absoluteRootPath:
                below[ancestor] += 1
    block = root
    if below:
        block = min(below, key=lambda p: (-below[p], p))
    middle = len(rows) // 2
    return {"payload_rows": len(rows), "load_building": str(rows[middle]), "load_block": str(block),
            "twirl_row": str(rows[middle + 1] if middle + 1 < len(rows) else rows[middle]),
            "payload_assets": sorted(set(a for assets in row_assets.values() for a in assets)),
            "payload_row_assets": row_assets}


def pick(scene, load):
    started = time.time()
    stage = Usd.Stage.Open(scene, load=Usd.Stage.LoadAll if load == "all" else Usd.Stage.LoadNone)
    if stage is None:
        raise SystemExit("cannot open " + scene)
    tally = Tally(stage)
    root = pick_root(stage, tally)
    rows = {"scene": os.path.abspath(scene), "load": load, "root": str(root) if root else None,
            "prims": tally.prims(root) if root else 0, "gprims": tally.gprims(root) if root else 0,
            "instances": tally.instances, "prototypes": len(tally.prototypes)}
    rows.update(pick_payload_rows(scene, root))
    rows["pick_s"] = round(time.time() - started, 1)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("scene")
    parser.add_argument("--load", choices=("all", "none"), default="none")
    parser.add_argument("--out")
    args = parser.parse_args()
    text = json.dumps(pick(args.scene, args.load), indent=1, sort_keys=True)
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        temporary = args.out + ".tmp"
        with open(temporary, "w") as f:
            f.write(text + "\n")
        os.replace(temporary, args.out)
    sys.stdout.write(text + "\n")


if __name__ == "__main__":
    main()
