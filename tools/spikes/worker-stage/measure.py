#!/usr/bin/env python3
# Times the ways a render worker can keep its own stage of the viewer's scene, and counts the
# change notices each way gives the worker stage (UsdImagingStageSceneIndex turns resynced paths
# into removed and re-added subtrees, changed-info paths into dirtied properties).
# usage: measure.py [--lobby <World_Lobby.usd>] [--prims N] [--repeats R] [--out <file.json>]
import argparse
import json
import statistics
import time

from pxr import Sdf, Tf, Usd

SPIKE_ATTR = "natron:spike"


def now():
    return time.perf_counter()


def anon(tag):
    return Sdf.Layer.CreateAnonymous(tag)


def open_stage(root):
    # The bindings make an empty session layer here; nothing edits it.
    return Usd.Stage.Open(root, Usd.Stage.LoadAll)


def copy_of(layer, tag):
    out = anon(tag)
    out.TransferContent(layer)
    return out


def generate(prims):
    layer = anon("gen")
    per_group = 100
    groups = max(1, prims // per_group)
    with Sdf.ChangeBlock():
        world = Sdf.PrimSpec(layer, "World", Sdf.SpecifierDef, "Xform")
        for g in range(groups):
            group = Sdf.PrimSpec(world, "g%05d" % g, Sdf.SpecifierDef, "Xform")
            for c in range(per_group - 1):
                cube = Sdf.PrimSpec(group, "c%03d" % c, Sdf.SpecifierDef, "Cube")
                size = Sdf.AttributeSpec(cube, "size", Sdf.ValueTypeNames.Double)
                size.default = 1.0
                op = Sdf.AttributeSpec(cube, "xformOp:translate", Sdf.ValueTypeNames.Double3)
                op.default = (float(c), float(g), 0.0)
                order = Sdf.AttributeSpec(cube, "xformOpOrder", Sdf.ValueTypeNames.TokenArray, Sdf.VariabilityUniform)
                order.default = ["xformOp:translate"]
    return layer, "/World/g00000/c000"


def lobby_reader(path):
    file_layer = Sdf.Layer.FindOrOpen(path)
    reader = anon("reader")
    reader.subLayerPaths = [file_layer.identifier]
    stage = open_stage(reader)
    target = None
    for prim in stage.Traverse():
        if prim.GetTypeName() == "Mesh":
            target = str(prim.GetPath())
            break
    return reader, file_layer, target


def edit_layer(prim_path, value):
    layer = anon("edit")
    spec = Sdf.CreatePrimInLayer(layer, prim_path)
    spec.specifier = Sdf.SpecifierOver
    attr = Sdf.AttributeSpec(spec, SPIKE_ATTR, Sdf.ValueTypeNames.Float)
    attr.default = value
    return layer


def with_one_edit(layer, prim_path, value):
    out = copy_of(layer, "recooked")
    spec = Sdf.CreatePrimInLayer(out, prim_path)
    if spec.specifier != Sdf.SpecifierDef:
        spec.specifier = Sdf.SpecifierOver
    attr = spec.attributes.get(SPIKE_ATTR) or Sdf.AttributeSpec(spec, SPIKE_ATTR, Sdf.ValueTypeNames.Float)
    attr.default = value
    return out


class Gui:
    # The viewer side as SceneStage builds it: a private root over a projections layer and the
    # node layers, strongest first.
    def __init__(self, stack):
        self.root = anon("natron-scene-root")
        self.projections = anon("natron-scene-projections")
        self.stack = list(stack)
        self.root.subLayerPaths = [self.projections.identifier] + [l.identifier for l in self.stack]
        self.root.pseudoRoot.SetInfo("upAxis", "Y")
        self.stage = open_stage(self.root)

    def sync(self, stack):
        t = now()
        with Sdf.ChangeBlock():
            self.root.subLayerPaths = [self.projections.identifier] + [l.identifier for l in stack]
        self.stack = list(stack)
        return now() - t


class Notices:
    def __init__(self, stage):
        self.stage = stage
        self.resynced = []
        self.info = []
        self.key = Tf.Notice.Register(Usd.Notice.ObjectsChanged, self.on, stage)

    def on(self, notice, sender):
        self.resynced.extend(notice.GetResyncedPaths())
        self.info.extend(notice.GetChangedInfoOnlyPaths())

    def take(self):
        # The stage scene index repopulates the subtree of a resynced prim path but only dirties
        # a resynced property, so the two are counted apart.
        prim_paths = [p for p in self.resynced if p.IsPrimPath() or p.IsAbsoluteRootPath()]
        tops = sorted(set(prim_paths))
        covered = []
        for p in tops:
            if not any(p.HasPrefix(c) for c in covered):
                covered.append(p)
        repopulated = 0
        for p in covered:
            if p.IsAbsoluteRootPath():
                repopulated += sum(1 for _ in self.stage.TraverseAll())
                continue
            prim = self.stage.GetPrimAtPath(p)
            if prim:
                repopulated += sum(1 for _ in Usd.PrimRange.AllPrims(prim))
        out = {
            "resyncedPrimPaths": len(prim_paths),
            "resyncedPropertyPaths": len(self.resynced) - len(prim_paths),
            "repopulatedPrims": repopulated,
            "infoOnly": len(self.info),
            "sample": [str(p) for p in (self.resynced + self.info)[:4]],
        }
        self.resynced, self.info = [], []
        return out


class Worker:
    """mode: 'full' copies every in-memory layer per snapshot, 'changed' copies only the layers
    whose identity changed, 'shared' sublayers the node layers themselves."""

    def __init__(self, gui, mode, in_place=True):
        self.mode = mode
        self.in_place = in_place
        self.root = anon("worker-root")
        self.projections = anon("worker-projections")
        self.mirrors = []
        self.sources = []
        self.stage = None
        self.notices = None
        self.gui = gui

    def copied(self, layer):
        # A file layer copied anonymously would lose its anchor; only a reload edits it, behind
        # the write barrier, so it is shared as it is.
        return self.mode != "shared" and layer.anonymous

    def snapshot(self):
        t = now()
        gui = self.gui
        snap = {
            "stack": list(gui.stack),
            "upAxis": gui.root.pseudoRoot.GetInfo("upAxis"),
            "projections": copy_of(gui.projections, "snap-projections"),
            "copies": {},
        }
        if self.mode != "shared":
            for i, layer in enumerate(snap["stack"]):
                if not self.copied(layer):
                    continue
                if self.mode == "changed" and i < len(self.sources) and self.sources[i] is not None and self.sources[i] == layer:
                    continue
                snap["copies"][i] = copy_of(layer, "snap")
        return snap, now() - t

    def apply(self, snap):
        t = now()
        stack = snap["stack"]
        ids = []
        mirrors = []
        with Sdf.ChangeBlock():
            for i, layer in enumerate(stack):
                if not self.copied(layer):
                    ids.append(layer.identifier)
                    mirrors.append(None)
                    continue
                mirror = self.mirrors[i] if i < len(self.mirrors) else None
                fresh = snap["copies"].get(i)
                if fresh is None:
                    mirrors.append(mirror)
                elif mirror is not None and self.in_place:
                    mirror.TransferContent(fresh)
                    mirrors.append(mirror)
                else:
                    mirrors.append(fresh)
                ids.append(mirrors[-1].identifier)
            self.projections.TransferContent(snap["projections"])
            wanted = [self.projections.identifier] + ids
            if list(self.root.subLayerPaths) != wanted:
                self.root.subLayerPaths = wanted
            self.root.pseudoRoot.SetInfo("upAxis", snap["upAxis"])
        self.mirrors = mirrors
        self.sources = list(stack)
        if self.stage is None:
            self.stage = open_stage(self.root)
            self.notices = Notices(self.stage)
        return now() - t


def median(values):
    return statistics.median(values) if values else 0.0


def ms(seconds):
    return round(seconds * 1000.0, 3)


def run_scene(name, base, edit_path, repeats, results):
    # A downstream node's small edit layer over the heavy one; the "recook" case replaces the
    # heavy layer itself, as editing the node that made it does.
    edit0 = edit_layer(edit_path, 0.0)
    for mode in ("full", "changed", "shared"):
        for in_place in ((True, False) if mode == "changed" else (True,)):
            label = mode + ("" if in_place else "-fresh")
            row = {"scene": name, "mode": label}
            locks, applies, guis = [], [], []
            notices_small = notices_recook = None
            first_lock = first_apply = None
            for r in range(repeats):
                gui = Gui([edit0, base])
                worker = Worker(gui, mode, in_place)
                snap, lock = worker.snapshot()
                apply_t = worker.apply(snap)
                traverse = now()
                prims = sum(1 for _ in worker.stage.Traverse())
                traverse = now() - traverse
                if r == 0:
                    first_lock, first_apply = [lock], [apply_t]
                    row["prims"] = prims
                    row["firstTraverseMs"] = ms(traverse)
                else:
                    first_lock.append(lock)
                    first_apply.append(apply_t)
                worker.notices.take()

                edit1 = edit_layer(edit_path, float(r + 1))
                guis.append(gui.sync([edit1, base]))
                snap, lock = worker.snapshot()
                locks.append(lock)
                applies.append(worker.apply(snap))
                n = worker.notices.take()
                notices_small = n

                recooked = with_one_edit(base, edit_path, float(r + 7))
                gui.sync([edit1, recooked])
                snap, lock_r = worker.snapshot()
                apply_r = worker.apply(snap)
                notices_recook = worker.notices.take()
                row.setdefault("_recookLock", []).append(lock_r)
                row.setdefault("_recookApply", []).append(apply_r)
                del worker, gui
            row["fullLockMs"] = ms(median(first_lock))
            row["fullApplyMs"] = ms(median(first_apply))
            row["editLockMs"] = ms(median(locks))
            row["editApplyMs"] = ms(median(applies))
            row["editGuiSyncMs"] = ms(median(guis))
            row["editNotices"] = notices_small
            row["recookLockMs"] = ms(median(row.pop("_recookLock")))
            row["recookApplyMs"] = ms(median(row.pop("_recookApply")))
            row["recookNotices"] = notices_recook
            results.append(row)
            print(json.dumps(row), flush=True)


def identical_transfer(base, results):
    root = anon("probe-root")
    mirror = copy_of(base, "probe-mirror")
    root.subLayerPaths = [mirror.identifier]
    stage = open_stage(root)
    notices = Notices(stage)
    t = now()
    mirror.TransferContent(copy_of(base, "same"))
    row = {"probe": "TransferContent of identical content", "ms": ms(now() - t), "notices": notices.take()}
    results.append(row)
    print(json.dumps(row), flush=True)
    t = now()
    stage2 = open_stage(root)
    opened = now() - t
    t = now()
    count = sum(1 for _ in stage2.Traverse())
    row = {"probe": "fresh stage on the same root", "openMs": ms(opened), "traverseMs": ms(now() - t), "prims": count}
    results.append(row)
    print(json.dumps(row), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lobby", default="/home/bosley/git/Natron/.local/World_Lobby.usd")
    parser.add_argument("--prims", type=int, default=100000)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    results = []

    reader, file_layer, target = lobby_reader(args.lobby)
    run_scene("lobby", reader, target, args.repeats, results)

    t = now()
    gen, gen_path = generate(args.prims)
    results.append({"probe": "generate %d prims" % args.prims, "ms": ms(now() - t)})
    t = now()
    copy_of(gen, "timing")
    results.append({"probe": "TransferContent of the generated layer into a new layer", "ms": ms(now() - t)})
    print(json.dumps(results[-1]), flush=True)
    run_scene("gen%dk" % (args.prims // 1000), gen, gen_path, args.repeats, results)
    identical_transfer(gen, results)

    if args.out:
        with open(args.out, "w") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
