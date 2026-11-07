"""Generates the heavy scene fixtures the scene-viewer spike and budgets run against.

  python3 tools/bench/heavy_scene_make.py [--out build/bench] [--prims 1000000]
          [--blocks 300] [--buildings-per-block 12] [--floors 16]

Needs the pxr Python module (the natron-dev image has it). Writes under --out:

  heavy_city.usda          payload city: /World > BlockN (group) > BM (assembly), one payload per building
  heavy_city_payloads/     one .usdc per building, each holding floors (component) of cube leaves
  heavy_flat.usdc          the same prim count as one flat file with no payloads

The city composes to about --prims once every payload is loaded and to a few thousand prims
before. Nine in ten buildings carry an extentsHint in the root layer; the rest have none, so a
viewer has to draw them as unbounded markers. Output is deterministic. Fixtures are never
committed; only this script is.
"""
import argparse
import os
import subprocess
import sys
import time

from pxr import Sdf, UsdGeom  # noqa: F401 (UsdGeom registers the schema)

HERE = os.path.dirname(os.path.abspath(__file__))
BUILDING_SIZE = 4.0
BUILDING_PITCH = 6.0
BLOCK_PITCH = 80.0
CITY_COLUMNS = 20


def _define(layer_or_spec, name, type_name, kind=None):
    prim = Sdf.PrimSpec(layer_or_spec, name, Sdf.SpecifierDef, type_name)
    if kind:
        prim.kind = kind
    return prim


def _translate(prim, x, y, z):
    attr = Sdf.AttributeSpec(prim, "xformOp:translate", Sdf.ValueTypeNames.Double3)
    attr.default = (float(x), float(y), float(z))
    order = Sdf.AttributeSpec(prim, "xformOpOrder", Sdf.ValueTypeNames.TokenArray, variability=Sdf.VariabilityUniform)
    order.default = ["xformOp:translate"]


def write_building(path, prims_per_building, floors):
    cubes = prims_per_building - 1 - floors
    base, extra = divmod(cubes, floors)
    layer = Sdf.Layer.CreateNew(path)
    with Sdf.ChangeBlock():
        layer.defaultPrim = "Building"
        layer.pseudoRoot.SetInfo("upAxis", "Y")
        root = _define(layer, "Building", "Xform", "assembly")
        for f in range(floors):
            floor = _define(root, "Floor%d" % f, "Xform", "component")
            _translate(floor, 0, f, 0)
            for c in range(base + (1 if f < extra else 0)):
                cube = _define(floor, "c%d" % c, "Cube")
                size = Sdf.AttributeSpec(cube, "size", Sdf.ValueTypeNames.Double)
                size.default = 0.5
                _translate(cube, c % 4, 0, (c // 4) % 4)
    layer.Save()


def make_city(out_dir, prims, blocks, buildings_per_block, floors):
    payload_dir = os.path.join(out_dir, "heavy_city_payloads")
    os.makedirs(payload_dir, exist_ok=True)
    buildings = blocks * buildings_per_block
    prims_per_building = (prims - 1 - blocks) // buildings
    if prims_per_building < floors + 2:
        raise SystemExit("--prims is too small for %d buildings of %d floors" % (buildings, floors))

    city_path = os.path.join(out_dir, "heavy_city.usda")
    layer = Sdf.Layer.CreateNew(city_path)
    with Sdf.ChangeBlock():
        layer.defaultPrim = "World"
        layer.pseudoRoot.SetInfo("upAxis", "Y")
        world = _define(layer, "World", "Xform", "group")
        for b in range(blocks):
            block = _define(world, "Block%d" % b, "Xform", "group")
            _translate(block, (b % CITY_COLUMNS) * BLOCK_PITCH, 0, (b // CITY_COLUMNS) * BLOCK_PITCH)
            for i in range(buildings_per_block):
                building = _define(block, "B%d" % i, "Xform", "assembly")
                _translate(building, (i % 4) * BUILDING_PITCH, 0, (i // 4) * BUILDING_PITCH)
                index = b * buildings_per_block + i
                payload_name = "b%d_%d.usdc" % (b, i)
                building.payloadList.Prepend(Sdf.Payload("./heavy_city_payloads/" + payload_name))
                if index % 10 != 0:
                    hint = Sdf.AttributeSpec(building, "extentsHint", Sdf.ValueTypeNames.Vector3fArray, variability=Sdf.VariabilityUniform)
                    height = float(floors)
                    hint.default = [(0.0, 0.0, 0.0), (BUILDING_SIZE, height, BUILDING_SIZE)]
    layer.Save()

    for b in range(blocks):
        for i in range(buildings_per_block):
            write_building(os.path.join(payload_dir, "b%d_%d.usdc" % (b, i)), prims_per_building, floors)
    print("wrote %s: %d payload files of %d prims" % (city_path, buildings, prims_per_building))


def make_flat(out_dir, prims):
    path = os.path.join(out_dir, "heavy_flat.usdc")
    subprocess.check_call([sys.executable, os.path.join(HERE, "scene_edit_bench.py"), "make-file", path, "--prims", str(prims)])


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default="build/bench")
    parser.add_argument("--prims", type=int, default=1000000)
    parser.add_argument("--blocks", type=int, default=300)
    parser.add_argument("--buildings-per-block", type=int, default=12)
    parser.add_argument("--floors", type=int, default=16)
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    start = time.time()
    make_city(args.out, args.prims, args.blocks, args.buildings_per_block, args.floors)
    make_flat(args.out, args.prims)
    print("generated in %.1f s" % (time.time() - start))


if __name__ == "__main__":
    main()
