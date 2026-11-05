#!/usr/bin/env python3
"""Write Tests/fixtures/ai/identity.dtmodel and upscale.dtmodel, the packages the tests install.

usage: make-test-dtmodel.py [--out <dir>] [--check]

Each package is laid out like a darktable-ai release asset: a zip whose
deflated members sit under a folder named after the package, holding
config.json and the models from make-test-models.py. identity.dtmodel
(test-identity) holds identity.onnx as model.onnx; upscale.dtmodel
(test-upscale) holds upscale2-nearest.onnx and upscale4-nearest.onnx as
model_x2.onnx and model_x4.onnx, as darktable's upscale packages name them.
Timestamps and attributes are fixed, so the same zlib writes byte-identical
files; the tests pin the results' sha256. --check writes nothing and exits
non-zero when a committed package differs.
"""

import argparse
import hashlib
import json
import os
import sys
import zipfile

def identity_config():
    return {
        "id": "test-identity",
        "name": "test identity",
        "description": "float32 identity, for Tests/Ai",
        "task": "denoise",
        "arch": "identity",
        "backend": "onnx",
        "version": "1.0",
        "tiling": True,
        "model_card": {
            "license": "MIT",
            "training_data_license": "none",
        },
        "attributes": {
            "input_sizes": [64],
        },
    }


def upscale_config():
    return {
        "id": "test-upscale",
        "name": "test upscale",
        "description": "float32 nearest-neighbour 2x and 4x, for Tests/Native",
        "task": "upscale",
        "arch": "nearest",
        "backend": "onnx",
        "version": "1.0",
        "tiling": True,
        "model_card": {
            "license": "MIT",
            "training_data_license": "none",
        },
        "attributes": {
            "input_sizes": [32],
        },
    }


# Output file: (package, config, [(member name, fixture model)]).
PACKAGES = {
    "identity.dtmodel": ("test-identity", identity_config, [("model.onnx", "identity.onnx")]),
    "upscale.dtmodel": ("test-upscale", upscale_config,
                        [("model_x2.onnx", "upscale2-nearest.onnx"), ("model_x4.onnx", "upscale4-nearest.onnx")]),
}


def member(package, name):
    info = zipfile.ZipInfo(package + "/" + name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = 0o644 << 16
    return info


def build(fixtures, out_name):
    package, config, models = PACKAGES[out_name]
    path = os.path.join(fixtures, out_name + ".tmp")
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(member(package, "config.json"), json.dumps(config(), indent=4) + "\n")
        for name, fixture in models:
            with open(os.path.join(fixtures, fixture), "rb") as f:
                archive.writestr(member(package, name), f.read())
    with open(path, "rb") as f:
        data = f.read()
    os.remove(path)
    return data


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=os.path.join(root, "Tests", "fixtures", "ai"))
    parser.add_argument("--check", action="store_true",
                        help="compare against the package in --out instead of writing it")
    args = parser.parse_args()

    stale = []
    for out_name in sorted(PACKAGES):
        data = build(args.out, out_name)
        path = os.path.join(args.out, out_name)
        if args.check:
            try:
                with open(path, "rb") as f:
                    if f.read() == data:
                        continue
            except FileNotFoundError:
                pass
            stale.append(out_name)
            continue
        with open(path, "wb") as f:
            f.write(data)
        print("%s: %d bytes, sha256 %s" % (path, len(data), hashlib.sha256(data).hexdigest()))

    if stale:
        print("out of date: " + ", ".join(stale), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
