#!/usr/bin/env python3
"""Write Tests/fixtures/ai/rawdenoise.dtmodel, the package RawNeuralDenoise's tests install.

usage: make-test-rawdenoise.py [--out <dir>] [--check]

Two stand-ins for RawNIND's Bayer model, shaped like it: a static [1, 4, 128, 128] input of
packed R, G, G, B planes and a [1, 3, 256, 256] output. Each 2x2 block of the output is its
block's (R, (G1 + G2) / 2, B), so the result depends on no neighbour and no tile placement.

- model_bayer.onnx returns that times 1000, under output_scale match_gain, which brings a grey
  field back to its input level.
- model_bayer_absolute.onnx returns it as is, under output_scale absolute.

The package is laid out like make-test-dtmodel.py's, and the output is byte-identical from run
to run; --check writes nothing and exits non-zero when the committed package differs.
"""

import argparse
import hashlib
import io
import json
import os
import sys
import zipfile

import onnx
from onnx import TensorProto, helper

OPSET = 17
IR_VERSION = 8
PRODUCER = "Natron tools/ai/make-test-rawdenoise.py"
PACKAGE = "test-rawdenoise"
TILE = 128

POLICY = {
    "input_kind": "bayer_v1",
    "bayer_orientation": "force_rggb",
    "edge_pad": "mirror_cropped",
    "wb_norm": "none",
}

CONFIG = {
    "id": PACKAGE,
    "name": "test raw denoise",
    "description": "per-block Bayer superpixel, for Tests/Native",
    "task": "rawdenoise",
    "arch": "superpixel",
    "backend": "onnx",
    "version": "1.0",
    "tiling": True,
    "model_card": {
        "license": "MIT",
        "training_data_license": "none",
    },
    "attributes": {
        "input_sizes": [TILE],
        "model_bayer": dict(POLICY, output_scale="match_gain"),
        "model_bayer_absolute": dict(POLICY, output_scale="absolute"),
    },
}


def superpixel(name, gain):
    # Output channel c * 4 + k of the 1x1 convolution lands on site k of colour c's 2x2 block
    # (DepthToSpace, CRD).
    rows = [[gain, 0., 0., 0.], [0., 0.5 * gain, 0.5 * gain, 0.], [0., 0., 0., gain]]
    weights = []
    for row in rows:
        for _ in range(4):
            weights.extend(row)
    w = helper.make_tensor("w", TensorProto.FLOAT, [12, 4, 1, 1], weights)
    nodes = [
        helper.make_node("Conv", ["input", "w"], ["blocks"]),
        helper.make_node("DepthToSpace", ["blocks"], ["output"], blocksize=2, mode="CRD"),
    ]
    graph = helper.make_graph(nodes, name,
                              [helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 4, TILE, TILE])],
                              [helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, 3, 2 * TILE, 2 * TILE])],
                              initializer=[w])
    m = helper.make_model(graph,
                          opset_imports=[helper.make_opsetid("", OPSET)],
                          producer_name=PRODUCER,
                          ir_version=IR_VERSION)
    onnx.checker.check_model(m, full_check=True)
    return m.SerializeToString()


def member(name):
    info = zipfile.ZipInfo(PACKAGE + "/" + name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = 0o644 << 16
    return info


def build():
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        archive.writestr(member("config.json"), json.dumps(CONFIG, indent=4) + "\n")
        archive.writestr(member("model_bayer.onnx"), superpixel("model_bayer", 1000.))
        archive.writestr(member("model_bayer_absolute.onnx"), superpixel("model_bayer_absolute", 1.))
    return buffer.getvalue()


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=os.path.join(root, "Tests", "fixtures", "ai"))
    parser.add_argument("--check", action="store_true",
                        help="compare against the package in --out instead of writing it")
    args = parser.parse_args()

    data = build()
    path = os.path.join(args.out, "rawdenoise.dtmodel")
    if args.check:
        try:
            with open(path, "rb") as f:
                if f.read() == data:
                    return 0
        except FileNotFoundError:
            pass
        print("out of date: rawdenoise.dtmodel", file=sys.stderr)
        return 1
    with open(path, "wb") as f:
        f.write(data)
    print("%s sha256 %s size %d" % (path, hashlib.sha256(data).hexdigest(), len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
