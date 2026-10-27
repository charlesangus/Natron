#!/usr/bin/env python3
"""Write Tests/fixtures/ai/identity.dtmodel, the package Tests/Ai installs.

usage: make-test-dtmodel.py [--out <dir>] [--check]

The package is laid out like a darktable-ai release asset: a zip whose
deflated members sit under a folder named after the package, holding
config.json and the model, here identity.onnx from make-test-models.py.
Timestamps and attributes are fixed, so the same zlib writes byte-identical
files; Tests/Ai/ModelDownloader_Test.cpp pins the result's sha256. --check
writes nothing and exits non-zero when the committed package differs.
"""

import argparse
import hashlib
import json
import os
import sys
import zipfile

PACKAGE = "test-identity"

CONFIG = {
    "id": PACKAGE,
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


def member(name):
    info = zipfile.ZipInfo(PACKAGE + "/" + name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = 0o644 << 16
    return info


def build(fixtures):
    with open(os.path.join(fixtures, "identity.onnx"), "rb") as f:
        model = f.read()
    path = os.path.join(fixtures, "identity.dtmodel.tmp")
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(member("config.json"), json.dumps(CONFIG, indent=4) + "\n")
        archive.writestr(member("model.onnx"), model)
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

    data = build(args.out)
    path = os.path.join(args.out, "identity.dtmodel")
    if args.check:
        try:
            with open(path, "rb") as f:
                if f.read() == data:
                    return 0
        except FileNotFoundError:
            pass
        print("out of date: identity.dtmodel", file=sys.stderr)
        return 1

    with open(path, "wb") as f:
        f.write(data)
    print("%s: %d bytes, sha256 %s" % (path, len(data), hashlib.sha256(data).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
