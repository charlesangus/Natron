#!/usr/bin/env python3
"""Export SEA-RAFT to ONNX and package it as optical-flow-searaft-m.dtmodel.

usage: export-flow-model.py --weights <spring-M.safetensors> --sea-raft <checkout>
                            [--out <dir>] [--verify]

The model is SEA-RAFT (github.com/princeton-vl/SEA-RAFT, BSD-3-Clause) at
commit SEA_RAFT_COMMIT, variant Tartan-C-T-TSKH-spring540x960-M with the
config/eval/spring-M.json architecture. --weights is that variant's
model.safetensors (Hugging Face MemorySlices/Tartan-C-T-TSKH-spring540x960-M,
sha256 WEIGHTS_SHA256), and --sea-raft a checkout of the repository at the
pinned commit.

model.onnx takes
  image1, image2  float32 [1, 3, H, W], RGB 0..255, H and W multiples of 8
  iterations      float32 [1], the refinement iterations (rounded down, 0 or more)
and returns
  flow            float32 [1, 2, H, W], the displacement from image1 to image2
                  in pixels, x then y.

The network is exported as three traced pieces (the feature, context and
correlation encoder, one refinement step, and the convex upsampler), which are
joined into one graph with an ONNX Loop around the step, so the iteration count
is a run-time input rather than an unrolled constant.

The package is a zip laid out like a darktable-ai release asset, with fixed
timestamps, so with the same weights and the same torch and onnx versions
(TORCH_VERSION, ONNX_VERSION) a second run writes a byte-identical file.
--verify runs the exported model with ONNX Runtime on the CPU. It fails
unless the flow matches SEA-RAFT's own PyTorch forward on a 384x256 pair, at
several iteration counts, and unless on a 1024x576 pair whose second image is
the first shifted, the flow matches the shift.

The export needs torch, safetensors, numpy, scipy (SEA-RAFT's utils import
it), huggingface_hub (its RAFT class derives from a hub mixin) and onnx;
--verify also needs onnxruntime. torchvision is not needed.
"""

import argparse
import hashlib
import io
import json
import os
import sys
import zipfile

SEA_RAFT_COMMIT = "9137517ba24e628442aec097d3afe71d03503b75"
WEIGHTS_SHA256 = "cb8cfbf14c5e0f6734b64add383708b7ff68cc6089a0007c67165d4761346102"
TORCH_VERSION = "2.15.0"
ONNX_VERSION = "1.23.2"
OPSET = 17
PACKAGE = "optical-flow-searaft-m"
MODEL_FILE = "model.onnx"

# config/eval/spring-M.json, minus the training-only fields.
ARCH = {
    "use_var": True,
    "var_min": 0,
    "var_max": 10,
    "pretrain": "resnet34",
    "initial_dim": 64,
    "block_dims": [64, 128, 256],
    "radius": 4,
    "dim": 128,
    "num_blocks": 2,
    "iters": 4,
}

TRAINING_DATA_LICENCE = (
    "TartanAir: CC BY 4.0; Spring: CC BY 4.0; FlyingChairs: research use only, no commercial use; "
    "FlyingThings3D: research use (terms unconfirmed); MPI Sintel: research use (MPI PS:License 1.0, unconfirmed); "
    "KITTI 2015: CC BY-NC-SA 3.0; HD1K: terms unconfirmed"
)


def config_json():
    return {
        "id": PACKAGE,
        "name": "SEA-RAFT M (Spring)",
        "description": "SEA-RAFT optical flow, medium size, fine-tuned on Spring. "
                       "Inputs image1, image2 (RGB 0..255) and iterations; output flow in pixels.",
        "task": "optical-flow",
        "arch": "sea-raft",
        "backend": "onnx",
        "version": "1.0",
        "tiling": False,
        "model_card": {
            "license": "BSD-3-Clause",
            "training_data_license": TRAINING_DATA_LICENCE,
            "source": "https://github.com/princeton-vl/SEA-RAFT/tree/" + SEA_RAFT_COMMIT,
            "weights": "https://huggingface.co/MemorySlices/Tartan-C-T-TSKH-spring540x960-M",
        },
        "attributes": {
            "multiple_of": 8,
            "default_iterations": ARCH["iters"],
        },
    }


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def load_model(weights, sea_raft):
    import torch
    from safetensors import safe_open
    from safetensors.torch import load_file

    sys.path.insert(0, os.path.join(sea_raft, "core"))
    import extractor
    import raft

    # The torchvision ImageNet initialisation is overwritten by the checkpoint, so skip its download.
    original = extractor.ResNetFPN._init_weights

    def no_pretrained(self, args):
        self.init_weight = False
        original(self, args)

    extractor.ResNetFPN._init_weights = no_pretrained
    model = raft.RAFT(argparse.Namespace(**ARCH))
    state = load_file(weights)
    # The checkpoint stores each downsample BatchNorm once, under its bn3 name, and maps the
    # downsample name to it in the metadata; restore those so the strict load finds every key.
    with safe_open(weights, "pt") as f:
        aliases = f.metadata() or {}
    for alias, stored in aliases.items():
        if stored in state:
            state[alias] = state[stored]
    model.load_state_dict(state, strict=True)
    model.eval()
    torch.set_grad_enabled(False)
    return model


def pieces(model):
    import torch
    import torch.nn as nn
    from corr import CorrBlock
    from utils.utils import coords_grid

    args = model.args

    class Encoder(nn.Module):
        def __init__(self):
            super().__init__()
            self.m = model

        def forward(self, image1, image2):
            m = self.m
            image1 = 2 * (image1 / 255.0) - 1.0
            image2 = 2 * (image2 / 255.0) - 1.0
            cnet = m.init_conv(m.cnet(torch.cat([image1, image2], dim=1)))
            net, context = torch.split(cnet, [args.dim, args.dim], dim=1)
            flow_8x = m.flow_head(net)[:, :2]
            corr = CorrBlock(m.fnet(image1), m.fnet(image2), args)
            return (net, context, flow_8x) + tuple(corr.corr_pyramid)

    class Step(nn.Module):
        def __init__(self):
            super().__init__()
            self.m = model

        def forward(self, net, context, flow_8x, corr0, corr1, corr2, corr3):
            m = self.m
            corr_fn = CorrBlock.__new__(CorrBlock)
            corr_fn.num_levels = args.corr_levels
            corr_fn.radius = args.corr_radius
            corr_fn.args = args
            corr_fn.corr_pyramid = [corr0, corr1, corr2, corr3]
            N, _, H, W = flow_8x.shape
            coords2 = coords_grid(N, H, W, device=flow_8x.device) + flow_8x
            corr = corr_fn(coords2)
            net = m.update_block(net, context, corr, flow_8x)
            flow_8x = flow_8x + m.flow_head(net)[:, :2]
            return net, flow_8x

    class Head(nn.Module):
        def __init__(self):
            super().__init__()
            self.m = model

        def forward(self, net, flow_8x):
            m = self.m
            weight = .25 * m.upsample_weight(net)
            info = torch.zeros_like(flow_8x)
            flow, _ = m.upsample_data(flow_8x, info, weight)
            return flow

    return Encoder().eval(), Step().eval(), Head().eval()


def export_piece(module, example, input_names, output_names, dynamic_axes):
    import onnx
    import torch

    buffer = io.BytesIO()
    torch.onnx.export(module, example, buffer, input_names=input_names, output_names=output_names,
                      dynamic_axes=dynamic_axes, opset_version=OPSET, do_constant_folding=True, dynamo=False)
    return onnx.load_from_string(buffer.getvalue())


def export_pieces(model):
    import torch

    encoder, step, head = pieces(model)
    # The correlation pyramid halves the 1/8-size map four times, so 128 is the smallest side it takes.
    h, w = 128, 192
    image = torch.rand(1, 3, h, w) * 255
    with torch.no_grad():
        enc_out = encoder(image, image)
    image_axes = {2: "height", 3: "width"}
    low_axes = {2: "h8", 3: "w8"}
    corr_names = ["corr%d" % i for i in range(4)]
    corr_axes = {name: {0: "hw8", 2: "ch%d" % i, 3: "cw%d" % i} for i, name in enumerate(corr_names)}

    def corr_dyn():
        return dict(corr_axes)

    enc = export_piece(encoder, (image, image), ["image1", "image2"], ["net", "context", "flow_8x"] + corr_names,
                       dict({"image1": image_axes, "image2": image_axes, "net": low_axes, "context": low_axes,
                             "flow_8x": low_axes}, **corr_dyn()))
    stp = export_piece(step, tuple(enc_out), ["net", "context", "flow_8x"] + corr_names, ["net_out", "flow_8x_out"],
                       dict({"net": low_axes, "context": low_axes, "flow_8x": low_axes, "net_out": low_axes,
                             "flow_8x_out": low_axes}, **corr_dyn()))
    hd = export_piece(head, (enc_out[0], enc_out[2]), ["net", "flow_8x"], ["flow"],
                      {"net": low_axes, "flow_8x": low_axes, "flow": image_axes})
    return enc, stp, hd


def rename_graph(graph, prefix, keep):
    """Prefix every value and node name in graph except the names in keep, which map to new names."""
    from onnx import AttributeProto

    def name(n):
        if n == "":
            return n
        if n in keep:
            return keep[n]
        return prefix + n

    for node in graph.node:
        for a in node.attribute:
            if a.type in (AttributeProto.GRAPH, AttributeProto.GRAPHS):
                sys.exit("%s: node %s holds a subgraph, which the joiner does not handle" % (prefix, node.name))
        node.name = prefix + node.name
        node.input[:] = [name(n) for n in node.input]
        node.output[:] = [name(n) for n in node.output]
    for init in graph.initializer:
        init.name = name(init.name)
    for value in list(graph.input) + list(graph.output):
        value.name = name(value.name)
    del graph.value_info[:]


def join(enc, stp, hd):
    import onnx
    from onnx import TensorProto, helper

    rename_graph(enc.graph, "enc/", {n: n for n in ["image1", "image2"]})
    enc_out = {o.name[len("enc/"):]: o.name for o in enc.graph.output}

    body_keep = {"net": "loop/net_in", "flow_8x": "loop/flow_in", "net_out": "loop/net_out",
                 "flow_8x_out": "loop/flow_out", "context": enc_out["context"]}
    for i in range(4):
        body_keep["corr%d" % i] = enc_out["corr%d" % i]
    rename_graph(stp.graph, "step/", body_keep)

    rename_graph(hd.graph, "head/", {"net": "loop/net_final", "flow_8x": "loop/flow_final", "flow": "flow"})

    float_low = lambda n, c: helper.make_tensor_value_info(n, TensorProto.FLOAT, [1, c, "h8", "w8"])
    body = helper.make_graph(
        list(stp.graph.node) + [helper.make_node("Identity", ["loop/cond_in"], ["loop/cond_out"], name="loop/cond")],
        "refine",
        [helper.make_tensor_value_info("loop/iter", TensorProto.INT64, []),
         helper.make_tensor_value_info("loop/cond_in", TensorProto.BOOL, []),
         float_low("loop/net_in", ARCH["dim"]), float_low("loop/flow_in", 2)],
        [helper.make_tensor_value_info("loop/cond_out", TensorProto.BOOL, []),
         float_low("loop/net_out", ARCH["dim"]), float_low("loop/flow_out", 2)])

    nodes = list(enc.graph.node)
    nodes += [
        helper.make_node("Constant", [], ["iters/zero"], name="iters/zero",
                         value=helper.make_tensor("iters/zero_v", TensorProto.FLOAT, [1], [0.0])),
        helper.make_node("Max", ["iterations", "iters/zero"], ["iters/nonneg"], name="iters/max"),
        helper.make_node("Floor", ["iters/nonneg"], ["iters/floor"], name="iters/floor"),
        helper.make_node("Cast", ["iters/floor"], ["iters/int"], name="iters/cast", to=TensorProto.INT64),
        helper.make_node("Constant", [], ["iters/scalar_shape"], name="iters/scalar_shape",
                         value=helper.make_tensor("iters/scalar_shape_v", TensorProto.INT64, [0], [])),
        helper.make_node("Reshape", ["iters/int", "iters/scalar_shape"], ["iters/count"], name="iters/reshape"),
        helper.make_node("Constant", [], ["iters/true"], name="iters/true",
                         value=helper.make_tensor("iters/true_v", TensorProto.BOOL, [], [True])),
        helper.make_node("Loop", ["iters/count", "iters/true", enc_out["net"], enc_out["flow_8x"]],
                         ["loop/net_final", "loop/flow_final"], name="loop", body=body),
    ]
    nodes += list(hd.graph.node)

    image = lambda n: helper.make_tensor_value_info(n, TensorProto.FLOAT, [1, 3, "height", "width"])
    graph = helper.make_graph(
        nodes, "sea_raft_m",
        [image("image1"), image("image2"), helper.make_tensor_value_info("iterations", TensorProto.FLOAT, [1])],
        [helper.make_tensor_value_info("flow", TensorProto.FLOAT, [1, 2, "height", "width"])],
        initializer=list(enc.graph.initializer) + list(stp.graph.initializer) + list(hd.graph.initializer))
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", OPSET)],
                              producer_name="Natron tools/ai/export-flow-model.py", producer_version="1")
    model.ir_version = 8
    model.doc_string = "SEA-RAFT M (Tartan-C-T-TSKH-spring540x960-M), BSD-3-Clause, commit " + SEA_RAFT_COMMIT
    onnx.checker.check_model(model, full_check=True)
    return model.SerializeToString()


def member(name):
    info = zipfile.ZipInfo(PACKAGE + "/" + name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = 0o644 << 16
    return info


def package(model_bytes, path):
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(member("config.json"), json.dumps(config_json(), indent=4) + "\n")
        archive.writestr(member(MODEL_FILE), model_bytes)


def shifted_pair(width, height, dx, dy):
    import numpy as np

    rng = np.random.default_rng(221)
    pad = 32
    base = rng.random((3, height + 2 * pad, width + 2 * pad), dtype=np.float32)
    # Smooth noise has structure at several scales, which a flow model can match.
    for _ in range(3):
        base = (base + np.roll(base, 1, 1) + np.roll(base, -1, 1) + np.roll(base, 1, 2) + np.roll(base, -1, 2)) / 5
    base = (base - base.min()) / (base.max() - base.min()) * 255
    first = base[:, pad:pad + height, pad:pad + width]
    second = base[:, pad - dy:pad - dy + height, pad - dx:pad - dx + width]
    return first[None].copy(), second[None].copy()


def verify(model_path, iterations_list):
    import time

    import numpy as np
    import onnxruntime as ort

    session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
    print("inputs:", [(i.name, i.type, i.shape) for i in session.get_inputs()])
    print("outputs:", [(o.name, o.type, o.shape) for o in session.get_outputs()])
    width, height, dx, dy = 1024, 576, 7, -4
    first, second = shifted_pair(width, height, dx, dy)
    ok = True
    margin = 32
    for iterations in iterations_list:
        start = time.monotonic()
        (flow,) = session.run(["flow"], {"image1": first, "image2": second,
                                         "iterations": np.array([iterations], dtype=np.float32)})
        elapsed = time.monotonic() - start
        inner = flow[0, :, margin:-margin, margin:-margin]
        err = np.hypot(inner[0] - dx, inner[1] - dy)
        print("iterations %d: %.2f s, mean flow (%.3f, %.3f), mean EPE %.3f, median %.3f"
              % (iterations, elapsed, inner[0].mean(), inner[1].mean(), err.mean(), np.median(err)))
        if iterations >= 2 and np.median(err) > 0.25:
            ok = False
    return ok


def matches_torch(model_path, model, iterations_list):
    """Whether the ONNX graph gives what SEA-RAFT's own forward gives at each iteration count."""
    import numpy as np
    import onnxruntime as ort
    import torch

    session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
    first, second = shifted_pair(384, 256, 3, 2)
    ok = True
    for iterations in iterations_list:
        (flow,) = session.run(["flow"], {"image1": first, "image2": second,
                                         "iterations": np.array([iterations], dtype=np.float32)})
        reference = model(torch.from_numpy(first), torch.from_numpy(second), iters=iterations,
                          test_mode=True)["final"].numpy()
        diff = float(np.abs(flow - reference).max())
        print("iterations %d: largest difference from the PyTorch model %.2e px" % (iterations, diff))
        if diff > 1e-3:
            ok = False
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--weights", required=True, help="Tartan-C-T-TSKH-spring540x960-M model.safetensors")
    parser.add_argument("--sea-raft", required=True, help="SEA-RAFT checkout at %s" % SEA_RAFT_COMMIT)
    parser.add_argument("--out", default=".", help="folder the package is written to")
    parser.add_argument("--verify", action="store_true", help="check the model with ONNX Runtime on the CPU")
    args = parser.parse_args()

    import onnx
    import torch

    if sha256_file(args.weights) != WEIGHTS_SHA256:
        sys.exit("%s: sha256 is not %s" % (args.weights, WEIGHTS_SHA256))
    for module, pinned in ((torch, TORCH_VERSION), (onnx, ONNX_VERSION)):
        if module.__version__.split("+")[0] != pinned:
            print("warning: %s %s, not %s: the package may not be byte-identical"
                  % (module.__name__, module.__version__, pinned), file=sys.stderr)

    model = load_model(args.weights, args.sea_raft)
    model_bytes = join(*export_pieces(model))
    os.makedirs(args.out, exist_ok=True)
    model_path = os.path.join(args.out, MODEL_FILE)
    with open(model_path, "wb") as f:
        f.write(model_bytes)
    archive = os.path.join(args.out, PACKAGE + ".dtmodel")
    package(model_bytes, archive)
    print("%s: %d bytes, sha256 %s" % (model_path, len(model_bytes), hashlib.sha256(model_bytes).hexdigest()))
    print("%s: %d bytes, sha256 %s" % (archive, os.path.getsize(archive), sha256_file(archive)))

    if args.verify and not matches_torch(model_path, model, [0, 1, 4, 8]):
        sys.exit("the ONNX model does not match the PyTorch model")
    if args.verify and not verify(model_path, [0, 1, 2, 4, 6, 8, 12]):
        sys.exit("the flow on the shifted pair does not match the shift")
    return 0


if __name__ == "__main__":
    sys.exit(main())
