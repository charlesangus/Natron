#!/usr/bin/env python3
"""Write the tiny ONNX models Tests/Ai runs against the real ONNX Runtime.

usage: make-test-models.py [--out <dir>] [--check]

The models are committed under Tests/fixtures/ai, so building and testing never
need the onnx package; only regenerating them does (pip install onnx). Output
is deterministic: the same onnx release writes byte-identical files. --check
writes nothing and exits non-zero when a committed model differs from what
the script would write.

Every image model takes one NCHW input, 'image', and returns one output,
'output'. They use opset 17 and IR version 8, which every ONNX Runtime the
loader accepts can read.
"""
import argparse
import os
import sys

import onnx
from onnx import TensorProto, helper

OPSET = 17
IR_VERSION = 8
PRODUCER = "Natron tools/ai/make-test-models.py"


def image_value(name, elem_type, height="height", width="width"):
    return helper.make_tensor_value_info(name, elem_type, [1, 3, height, width])


def model(name, nodes, inputs, outputs, initializers=()):
    graph = helper.make_graph(nodes, name, inputs, outputs, initializer=list(initializers))
    m = helper.make_model(graph,
                          opset_imports=[helper.make_opsetid("", OPSET)],
                          producer_name=PRODUCER,
                          ir_version=IR_VERSION)
    onnx.checker.check_model(m, full_check=True)
    return m


def identity(elem_type, name):
    return model(name,
                 [helper.make_node("Identity", ["image"], ["output"])],
                 [image_value("image", elem_type)],
                 [image_value("output", elem_type)])


def identity_fixed():
    # A model with a fixed 64x64 input: the tiler clamps its tiles to that size and pads smaller ones.
    return model("identity_64",
                 [helper.make_node("Identity", ["image"], ["output"])],
                 [image_value("image", TensorProto.FLOAT, 64, 64)],
                 [image_value("output", TensorProto.FLOAT, 64, 64)])


def scale2(elem_type, name):
    two = helper.make_tensor("two", elem_type, [], [2.0])
    return model(name,
                 [helper.make_node("Mul", ["image", "two"], ["output"])],
                 [image_value("image", elem_type)],
                 [image_value("output", elem_type)],
                 [two])


def upscale_nearest(factor):
    # Asymmetric coordinates with floor rounding: output pixel (x, y) is input pixel
    # (x/factor, y/factor).
    scales = helper.make_tensor("scales", TensorProto.FLOAT, [4], [1.0, 1.0, float(factor), float(factor)])
    resize = helper.make_node("Resize", ["image", "", "scales"], ["output"],
                              mode="nearest",
                              coordinate_transformation_mode="asymmetric",
                              nearest_mode="floor")
    return model("upscale%d_nearest" % factor,
                 [resize],
                 [image_value("image", TensorProto.FLOAT)],
                 [image_value("output", TensorProto.FLOAT, "height%d" % factor, "width%d" % factor)],
                 [scales])


def slow_loop():
    # A Loop that, in practice, never ends: each iteration is one cheap node, so ONNX Runtime
    # checks its terminate flag often and an abort must be what stops the run.
    body = helper.make_graph(
        [helper.make_node("Identity", ["cond_in"], ["cond_out"]),
         helper.make_node("Identity", ["value_in"], ["value_out"])],
        "slow_loop_body",
        [helper.make_tensor_value_info("iteration", TensorProto.INT64, []),
         helper.make_tensor_value_info("cond_in", TensorProto.BOOL, []),
         helper.make_tensor_value_info("value_in", TensorProto.FLOAT, ["n"])],
        [helper.make_tensor_value_info("cond_out", TensorProto.BOOL, []),
         helper.make_tensor_value_info("value_out", TensorProto.FLOAT, ["n"])])
    trips = helper.make_tensor("trips", TensorProto.INT64, [], [1 << 50])
    loop = helper.make_node("Loop", ["trips", "", "input"], ["output"], body=body)
    return model("slow_loop",
                 [loop],
                 [helper.make_tensor_value_info("input", TensorProto.FLOAT, ["n"])],
                 [helper.make_tensor_value_info("output", TensorProto.FLOAT, ["n"])],
                 [trips])


MODELS = {
    "identity.onnx": lambda: identity(TensorProto.FLOAT, "identity"),
    "identity-fp16.onnx": lambda: identity(TensorProto.FLOAT16, "identity_fp16"),
    "identity-64.onnx": identity_fixed,
    "scale2.onnx": lambda: scale2(TensorProto.FLOAT, "scale2"),
    "scale2-fp16.onnx": lambda: scale2(TensorProto.FLOAT16, "scale2_fp16"),
    "upscale2-nearest.onnx": lambda: upscale_nearest(2),
    "upscale4-nearest.onnx": lambda: upscale_nearest(4),
    "slow-loop.onnx": slow_loop,
}


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=os.path.join(root, "Tests", "fixtures", "ai"))
    parser.add_argument("--check", action="store_true",
                        help="compare against the files in --out instead of writing them")
    args = parser.parse_args()

    stale = []
    os.makedirs(args.out, exist_ok=True)
    for name, make in sorted(MODELS.items()):
        data = make().SerializeToString(deterministic=True)
        path = os.path.join(args.out, name)
        if args.check:
            try:
                with open(path, "rb") as f:
                    if f.read() != data:
                        stale.append(name)
            except FileNotFoundError:
                stale.append(name)
            continue
        with open(path, "wb") as f:
            f.write(data)
        print(f"{path}: {len(data)} bytes")

    if stale:
        print("out of date: " + ", ".join(stale), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
