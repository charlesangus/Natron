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


# Stand-ins for darktable-ai's two-graph "mask" models, at a 64-pixel input size. Their "features"
# are the image's channel mean, and their mask is that feature thresholded, so a bright object on a
# dark ground segments as itself wherever the prompt is.
SEG_SIZE = 64


def seg_prompt_inputs():
    return [helper.make_tensor_value_info("point_coords", TensorProto.FLOAT, [1, "points", 2]),
            helper.make_tensor_value_info("point_labels", TensorProto.FLOAT, [1, "points"])]


def segnext_encoder():
    return model("fake_segnext_encoder",
                 [helper.make_node("ReduceMean", ["image"], ["image_feats"], axes=[1], keepdims=1)],
                 [image_value("image", TensorProto.FLOAT, SEG_SIZE, SEG_SIZE)],
                 [helper.make_tensor_value_info("image_feats", TensorProto.FLOAT, [1, 1, SEG_SIZE, SEG_SIZE])])


def segnext_decoder():
    half = helper.make_tensor("half", TensorProto.FLOAT, [], [0.5])
    gain = helper.make_tensor("gain", TensorProto.FLOAT, [], [40.0])
    return model("fake_segnext_decoder",
                 [helper.make_node("Sub", ["image_feats", "half"], ["centred"]),
                  helper.make_node("Mul", ["centred", "gain"], ["logits"]),
                  helper.make_node("Sigmoid", ["logits"], ["mask"])],
                 [helper.make_tensor_value_info("image_feats", TensorProto.FLOAT, [1, 1, SEG_SIZE, SEG_SIZE])]
                 + seg_prompt_inputs()
                 + [helper.make_tensor_value_info("prev_mask", TensorProto.FLOAT, [1, 1, SEG_SIZE, SEG_SIZE])],
                 [helper.make_tensor_value_info("mask", TensorProto.FLOAT, [1, 1, SEG_SIZE, SEG_SIZE])],
                 [half, gain])


def sam_encoder():
    # Outputs in another order, and under a longer name, than the decoder takes them.
    return model("fake_sam_encoder",
                 [helper.make_node("Identity", ["image"], ["high_res_feats_0"]),
                  helper.make_node("ReduceMean", ["image"], ["image_embeddings"], axes=[1], keepdims=1)],
                 [image_value("image", TensorProto.FLOAT, SEG_SIZE, SEG_SIZE)],
                 [image_value("high_res_feats_0", TensorProto.FLOAT, SEG_SIZE, SEG_SIZE),
                  helper.make_tensor_value_info("image_embeddings", TensorProto.FLOAT, [1, 1, SEG_SIZE, SEG_SIZE])])


def sam_decoder():
    # Three logit masks: the inverse, the object and nothing; the IoU head always picks the second.
    low = SEG_SIZE // 4
    neg = helper.make_tensor("neg", TensorProto.FLOAT, [], [-10.0])
    pos = helper.make_tensor("pos", TensorProto.FLOAT, [], [10.0])
    zero = helper.make_tensor("zero", TensorProto.FLOAT, [], [0.0])
    iou = helper.make_tensor("iou", TensorProto.FLOAT, [1, 3], [0.2, 0.9, 0.5])
    return model("fake_sam_decoder",
                 [helper.make_node("Mul", ["image_embed", "neg"], ["inverse"]),
                  helper.make_node("Mul", ["image_embed", "pos"], ["object"]),
                  helper.make_node("Mul", ["image_embed", "zero"], ["nothing"]),
                  helper.make_node("Concat", ["inverse", "object", "nothing"], ["masks"], axis=1),
                  helper.make_node("Identity", ["iou"], ["iou_predictions"]),
                  helper.make_node("MaxPool", ["masks"], ["low_res_masks"], kernel_shape=[4, 4], strides=[4, 4])],
                 [helper.make_tensor_value_info("image_embed", TensorProto.FLOAT, [1, 1, SEG_SIZE, SEG_SIZE]),
                  image_value("high_res_feats_0", TensorProto.FLOAT, SEG_SIZE, SEG_SIZE)]
                 + seg_prompt_inputs()
                 + [helper.make_tensor_value_info("mask_input", TensorProto.FLOAT, [1, 1, low, low]),
                    helper.make_tensor_value_info("has_mask_input", TensorProto.FLOAT, [1])],
                 [helper.make_tensor_value_info("masks", TensorProto.FLOAT, [1, 3, SEG_SIZE, SEG_SIZE]),
                  helper.make_tensor_value_info("iou_predictions", TensorProto.FLOAT, [1, 3]),
                  helper.make_tensor_value_info("low_res_masks", TensorProto.FLOAT, [1, 3, low, low])],
                 [neg, pos, zero, iou])


# The constant flow flow-constant.onnx returns, in pixels: dx then dy.
FLOW_DX = 2.5
FLOW_DY = -1.5


def flow_constant():
    # A RAFT-shaped model: two images in, [1, 2, height, width] flow out, the same everywhere.
    # Only the images' size reaches the output, so tests check geometry and not quality.
    tail = helper.make_tensor("tail_start", TensorProto.INT64, [1], [2])
    tail_end = helper.make_tensor("tail_end", TensorProto.INT64, [1], [4])
    head = helper.make_tensor("flow_head", TensorProto.INT64, [2], [1, 2])
    zero = helper.make_tensor("zero", TensorProto.FLOAT, [1], [0.0])
    offset = helper.make_tensor("offset", TensorProto.FLOAT, [1, 2, 1, 1], [FLOW_DX, FLOW_DY])
    return model("flow_constant",
                 [helper.make_node("Shape", ["image1"], ["dims"]),
                  helper.make_node("Slice", ["dims", "tail_start", "tail_end"], ["size"]),
                  helper.make_node("Concat", ["flow_head", "size"], ["flow_dims"], axis=0),
                  helper.make_node("ConstantOfShape", ["flow_dims"], ["zeros"], value=zero),
                  helper.make_node("Add", ["zeros", "offset"], ["flow"])],
                 [image_value("image1", TensorProto.FLOAT), image_value("image2", TensorProto.FLOAT)],
                 [helper.make_tensor_value_info("flow", TensorProto.FLOAT, [1, 2, "height", "width"])],
                 [tail, tail_end, head, offset])

# flow-order.onnx's gain per unit of brightness difference: dx and dy per 1 of (mean image2 - mean image1)
# at the model's 0..255 input scale.
FLOW_ORDER_GAIN_DX = 1.0 / 255.0
FLOW_ORDER_GAIN_DY = 2.0 / 255.0


def flow_order():
    # The flow is the same everywhere and equals gain * (mean of image2 - mean of image1), so
    # feeding the images in the other order flips its sign.
    zero = helper.make_tensor("zero", TensorProto.FLOAT, [1], [0.0])
    gain = helper.make_tensor("gain", TensorProto.FLOAT, [1, 2, 1, 1], [FLOW_ORDER_GAIN_DX, FLOW_ORDER_GAIN_DY])
    tail = helper.make_tensor("tail_start", TensorProto.INT64, [1], [2])
    tail_end = helper.make_tensor("tail_end", TensorProto.INT64, [1], [4])
    head = helper.make_tensor("flow_head", TensorProto.INT64, [2], [1, 2])
    return model("flow_order",
                 [helper.make_node("Shape", ["image1"], ["dims"]),
                  helper.make_node("Slice", ["dims", "tail_start", "tail_end"], ["size"]),
                  helper.make_node("Concat", ["flow_head", "size"], ["flow_dims"], axis=0),
                  helper.make_node("ConstantOfShape", ["flow_dims"], ["zeros"], value=zero),
                  helper.make_node("ReduceMean", ["image1"], ["mean1"], axes=[1, 2, 3], keepdims=1),
                  helper.make_node("ReduceMean", ["image2"], ["mean2"], axes=[1, 2, 3], keepdims=1),
                  helper.make_node("Sub", ["mean2", "mean1"], ["difference"]),
                  helper.make_node("Mul", ["difference", "gain"], ["scaled"]),
                  helper.make_node("Add", ["zeros", "scaled"], ["flow"])],
                 [image_value("image1", TensorProto.FLOAT), image_value("image2", TensorProto.FLOAT)],
                 [helper.make_tensor_value_info("flow", TensorProto.FLOAT, [1, 2, "height", "width"])],
                 [tail, tail_end, head, gain])


def flow_position():
    # dx is the column and dy the row of each pixel of the padded input, so a flow cropped at the
    # wrong offset, or read from the wrong channel, differs from the pixel's own coordinates.
    zero = helper.make_tensor("zero_f", TensorProto.FLOAT, [], [0.0])
    one = helper.make_tensor("one_f", TensorProto.FLOAT, [], [1.0])
    height_start = helper.make_tensor("height_start", TensorProto.INT64, [1], [2])
    height_end = helper.make_tensor("height_end", TensorProto.INT64, [1], [3])
    width_start = helper.make_tensor("width_start", TensorProto.INT64, [1], [3])
    width_end = helper.make_tensor("width_end", TensorProto.INT64, [1], [4])
    axes = helper.make_tensor("squeeze_axes", TensorProto.INT64, [1], [0])
    column_shape = helper.make_tensor("column_shape", TensorProto.INT64, [4], [1, 1, 1, -1])
    row_shape = helper.make_tensor("row_shape", TensorProto.INT64, [4], [1, 1, -1, 1])
    only_x = helper.make_tensor("only_x", TensorProto.FLOAT, [1, 2, 1, 1], [1.0, 0.0])
    only_y = helper.make_tensor("only_y", TensorProto.FLOAT, [1, 2, 1, 1], [0.0, 1.0])
    return model("flow_position",
                 [helper.make_node("Shape", ["image1"], ["dims"]),
                  helper.make_node("Slice", ["dims", "height_start", "height_end"], ["height_1"]),
                  helper.make_node("Slice", ["dims", "width_start", "width_end"], ["width_1"]),
                  helper.make_node("Squeeze", ["height_1", "squeeze_axes"], ["height"]),
                  helper.make_node("Squeeze", ["width_1", "squeeze_axes"], ["width"]),
                  helper.make_node("Cast", ["height"], ["height_f"], to=TensorProto.FLOAT),
                  helper.make_node("Cast", ["width"], ["width_f"], to=TensorProto.FLOAT),
                  helper.make_node("Range", ["zero_f", "width_f", "one_f"], ["columns"]),
                  helper.make_node("Range", ["zero_f", "height_f", "one_f"], ["rows"]),
                  helper.make_node("Reshape", ["columns", "column_shape"], ["columns_4d"]),
                  helper.make_node("Reshape", ["rows", "row_shape"], ["rows_4d"]),
                  helper.make_node("Mul", ["columns_4d", "only_x"], ["x_part"]),
                  helper.make_node("Mul", ["rows_4d", "only_y"], ["y_part"]),
                  helper.make_node("Add", ["x_part", "y_part"], ["flow"])],
                 [image_value("image1", TensorProto.FLOAT), image_value("image2", TensorProto.FLOAT)],
                 [helper.make_tensor_value_info("flow", TensorProto.FLOAT, [1, 2, "height", "width"])],
                 [zero, one, height_start, height_end, width_start, width_end, axes, column_shape, row_shape,
                  only_x, only_y])


# flow-iterations.onnx's flow per refinement iteration it is given: dx then dy.
FLOW_ITERATIONS_DX = 1.0
FLOW_ITERATIONS_DY = 2.0


def flow_iterations():
    # The two images and an 'iterations' float [1] input, as the packaged SEA-RAFT takes; the flow is
    # the same everywhere and proportional to the iteration count, so a test sees what reached it.
    tail = helper.make_tensor("tail_start", TensorProto.INT64, [1], [2])
    tail_end = helper.make_tensor("tail_end", TensorProto.INT64, [1], [4])
    head = helper.make_tensor("flow_head", TensorProto.INT64, [2], [1, 2])
    zero = helper.make_tensor("zero", TensorProto.FLOAT, [1], [0.0])
    per = helper.make_tensor("per_iteration", TensorProto.FLOAT, [1, 2, 1, 1], [FLOW_ITERATIONS_DX, FLOW_ITERATIONS_DY])
    return model("flow_iterations",
                 [helper.make_node("Shape", ["image1"], ["dims"]),
                  helper.make_node("Slice", ["dims", "tail_start", "tail_end"], ["size"]),
                  helper.make_node("Concat", ["flow_head", "size"], ["flow_dims"], axis=0),
                  helper.make_node("ConstantOfShape", ["flow_dims"], ["zeros"], value=zero),
                  helper.make_node("Mul", ["iterations", "per_iteration"], ["offset"]),
                  helper.make_node("Add", ["zeros", "offset"], ["flow"])],
                 [image_value("image1", TensorProto.FLOAT), image_value("image2", TensorProto.FLOAT),
                  helper.make_tensor_value_info("iterations", TensorProto.FLOAT, [1])],
                 [helper.make_tensor_value_info("flow", TensorProto.FLOAT, [1, 2, "height", "width"])],
                 [tail, tail_end, head, per])


MODELS = {
    "identity.onnx": lambda: identity(TensorProto.FLOAT, "identity"),
    "identity-fp16.onnx": lambda: identity(TensorProto.FLOAT16, "identity_fp16"),
    "identity-64.onnx": identity_fixed,
    "scale2.onnx": lambda: scale2(TensorProto.FLOAT, "scale2"),
    "scale2-fp16.onnx": lambda: scale2(TensorProto.FLOAT16, "scale2_fp16"),
    "upscale2-nearest.onnx": lambda: upscale_nearest(2),
    "upscale4-nearest.onnx": lambda: upscale_nearest(4),
    "slow-loop.onnx": slow_loop,
    "flow-constant.onnx": flow_constant,
    "flow-order.onnx": flow_order,
    "flow-position.onnx": flow_position,
    "flow-iterations.onnx": flow_iterations,
    "seg-segnext-encoder.onnx": segnext_encoder,
    "seg-segnext-decoder.onnx": segnext_decoder,
    "seg-sam-encoder.onnx": sam_encoder,
    "seg-sam-decoder.onnx": sam_decoder,
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
