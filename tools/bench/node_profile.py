# Renders one native node, alone or in a short chain, at UHD float with NATRON_RENDER_PROFILE on, so
# that the profile holds that node's CPU task times or its GPU region's kernel times.
# Run inside NatronRenderer; tools/bench/node_profile.sh drives it and summarises the result.
#
#   NP_NODE=grade NP_CHAIN=8 NP_FRAMES=6 NatronRenderer -b tools/bench/node_profile.py
#
# Environment:
#   NP_NODE    key of a node in NODES (below)
#   NP_CHAIN   copies of the node in series (default 8). A generator is never chained. A chain
#              lets the placement see a region worth its transfers; kernel time is then per chain.
#   NP_FRAMES  frames rendered one at a time (default 6); the first is cold and is dropped later
#   NP_WORK    directory the frames are written to
#   NP_OCIO_CONFIG  path of a config.ocio the project uses (as its Custom config) before any node is
#              created. Unset, the project keeps its default, the built-in ACES studio config
#              (ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5), which the ocio_* nodes below are
#              written for. Only builds with the OCIO nodes have the project knobs.
#
# A key whose plugin the running binary lacks fails at creation, so select only keys it has.

import os

import NatronEngine

NODE = os.environ["NP_NODE"]
CHAIN = int(os.environ.get("NP_CHAIN", "8"))
FRAMES = int(os.environ.get("NP_FRAMES", "6"))
WORK = os.environ.get("NP_WORK", os.path.join(os.getcwd(), "build", "bench", "work"))
LAST_FRAME = FRAMES + 1


def make(plugin_id, name):
    props = {"CreateNodeArgsPropNodeInitialName": NatronEngine.StringNodeCreationProperty(name)}
    node = app.createNode(plugin_id, -1, None, props)
    if node is None:
        raise RuntimeError("could not create " + plugin_id)
    return node


def param(node, name):
    prm = node.getParam(name)
    if prm is None:
        raise RuntimeError("%s has no param %s" % (node.getPluginID(), name))
    return prm


def animate(prm, base, per_frame):
    for dim in range(len(base)):
        for f in range(1, LAST_FRAME + 1):
            prm.setValueAtTime(base[dim] + per_frame[dim] * f, f, dim)


# key: (plugin id, params, generator). Params leave the node doing work: an identity would be
# passed through. A tuple sets several dimensions of one parameter.
NODES = {
    "grade": ("net.sf.openfx.GradePlugin", {"multiply": (1.1, 1.05, 1.0, 1.0), "offset": (0.01, 0.0, 0.0, 0.0)}, False),
    "colorcorrect": ("net.sf.openfx.ColorCorrectPlugin",
                     {"MasterSaturation": (1.2, 1.2, 1.2, 1.0), "MasterGain": (1.1, 1.1, 1.1, 1.0)}, False),
    "invert": ("net.sf.openfx.Invert", {}, False),
    "clamp": ("net.sf.openfx.Clamp", {"minimum": (0.1, 0.1, 0.1, 0.1), "maximum": (0.9, 0.9, 0.9, 0.9)}, False),
    "add": ("net.sf.openfx.AddPlugin", {"value": (0.05, 0.05, 0.05, 0.0)}, False),
    "multiply": ("net.sf.openfx.MultiplyPlugin", {"value": (1.1, 1.1, 1.1, 1.0)}, False),
    "gamma": ("net.sf.openfx.GammaPlugin", {"value": (1.2, 1.2, 1.2, 1.0)}, False),
    "saturation": ("net.sf.openfx.SaturationPlugin", {"saturation": 1.3}, False),
    "colorlookup": ("net.sf.openfx.ColorLookupPlugin", {"clampBlack": True, "clampWhite": True}, False),
    "keyer": ("net.sf.openfx.KeyerPlugin", {"mode": 1, "keyColor": (0.5, 0.5, 0.5, 1.0)}, False),
    "chromakeyer": ("net.sf.openfx.ChromaKeyerPlugin", {"keyColor": (0.1, 0.9, 0.1, 1.0)}, False),
    "erode": ("net.sf.cimg.CImgErode", {"size": (6, 6)}, False),
    "erode21": ("net.sf.cimg.CImgErode", {"size": (21, 21)}, False),
    "dilate": ("net.sf.cimg.CImgDilate", {"size": (6, 6)}, False),
    "transform": ("net.sf.openfx.TransformPlugin",
                  {"translate": (10.5, 7.25), "rotate": 1.0, "center": (1920.0, 1080.0)}, False),
    "transform_mb1": ("net.sf.openfx.TransformPlugin", {"center": (1920.0, 1080.0), "motionBlur": 1.0, "shutter": 1.0}, False),
    "transform_mb4": ("net.sf.openfx.TransformPlugin", {"center": (1920.0, 1080.0), "motionBlur": 4.0, "shutter": 1.0}, False),
    "blur_fir3": ("net.sf.cimg.CImgBlur", {"size": (7.2, 7.2)}, False),
    "blur_fir25": ("net.sf.cimg.CImgBlur", {"size": (60.0, 60.0)}, False),
    "blur_fir100": ("net.sf.cimg.CImgBlur", {"size": (240.0, 240.0)}, False),
    "blur_qg100": ("net.sf.cimg.CImgBlur", {"size": (240.0, 240.0), "filter": 0}, False),
    "blur_gauss100": ("net.sf.cimg.CImgBlur", {"size": (240.0, 240.0), "filter": 1}, False),
    "blur_box100": ("net.sf.cimg.CImgBlur", {"size": (240.0, 240.0), "filter": 2}, False),
    "blur": ("net.sf.cimg.CImgBlur", {"size": (12.0, 12.0)}, False),
    "iblur": ("IBlur", {"size": (60.0, 60.0)}, False),
    "ierode": ("IErode", {"size": (6.0, 6.0)}, False),
    "idilate": ("IDilate", {"size": (6.0, 6.0)}, False),
    "dissolve": ("net.sf.openfx.DissolvePlugin", {"which": 0.5}, False),
    "crop": ("net.sf.openfx.CropPlugin",
             {"bottomLeft": (200.0, 150.0), "size": (3400.0, 1900.0), "softness": 20.0}, False),
    "reformat": ("net.sf.openfx.Reformat", {"reformatType": 1, "boxFixed": True, "boxSize": (1920, 1080)}, False),
    "position": ("net.sf.openfx.Position", {"translate": (12.6, 7.4)}, False),
    "constant": ("net.sf.openfx.ConstantPlugin", {}, True),
    "solid": ("net.sf.openfx.Solid", {}, True),
    "checkerboard": ("net.sf.openfx.CheckerBoardPlugin", {}, True),
    "noise": ("Noise", {"type": "simplex", "fractal": "fbm", "octaves": 6, "lacunarity": 2.0, "gain": 0.5,
                        "noiseSize": (64.0, 64.0), "warpType": "perlin", "warpAmount": 0.5}, True),
    "lensdistortion": ("LensDistortion", {"model": "Nuke", "k1": 0.1, "k2": 0.02, "direction": "Distort"}, False),
    "merge": ("Merge", {"operation": "over"}, False),
    "merge_plus": ("Merge", {"operation": "plus"}, False),
    "ocio_colorspace": ("OCIOColorSpace", {"in_colorspace": "ACES2065-1", "out_colorspace": "ACEScg"}, False),
    "ocio_display": ("OCIODisplay", {"in_colorspace": "ACEScg", "display": "sRGB - Display",
                                     "view": "ACES 2.0 - SDR 100 nits (Rec.709)", "gain": 1.1}, False),
    "ocio_cdl": ("OCIOCDLTransform", {"slope": (1.1, 0.9, 1.0), "offset": (0.01, 0.0, -0.01),
                                      "power": (1.1, 1.0, 0.9), "saturation": 1.2}, False),
    "ocio_file": ("OCIOFileTransform", {"file": "@LUT@"}, False),
    "ocio_look": ("OCIOLookTransform", {"in_colorspace": "ACEScg", "out_colorspace": "ACEScg",
                                        "single_look": True, "look": "@LOOK@"}, False),
}


# Nodes whose input 1 is fed a second source.
# Merge: input 0 is B, fed the chain; input 1 is A, fed the second source.
TWO_INPUTS = ("dissolve", "merge", "merge_plus")

# Nodes whose Map input (the number given) is fed a ramp whose alpha, the Map's default channel,
# runs from 0 to 1 across the frame, so the size varies from Size At Black to Size.
MAP_INPUTS = {"iblur": 2, "ierode": 2, "idilate": 2}


def use_ocio_config():
    path = os.environ.get("NP_OCIO_CONFIG")
    if not path:
        return
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise RuntimeError("NP_OCIO_CONFIG is not a file: " + path)
    app.getProjectParam("ocioConfigFile").set(path)
    app.getProjectParam("ocioConfig").set("Custom config")


def write_cube():
    # A 17^3 LUT, red fastest, that bends every channel and mixes in the others: never an identity.
    path = os.path.join(WORK, "np_lut17.cube")
    os.makedirs(WORK, exist_ok=True)
    n = 17
    with open(path, "w") as f:
        f.write("LUT_3D_SIZE %d\n" % n)
        for b in range(n):
            for g in range(n):
                for r in range(n):
                    x, y, z = r / (n - 1.0), g / (n - 1.0), b / (n - 1.0)
                    f.write("%.6f %.6f %.6f\n" % (min(1.0, x ** 0.8 * 0.9 + y * 0.1),
                                                   min(1.0, y ** 0.9 * 0.9 + z * 0.1),
                                                   min(1.0, z ** 1.1 * 0.9 + x * 0.1)))
    return path


def first_look(node):
    for option in param(node, "look").getOptions():
        if option and option.lower() != "none":
            return option
    raise RuntimeError("the OCIO config has no looks")


def configure(name, index):
    plugin_id, values, generator = NODES[name]
    node = make(plugin_id, "np_%s_%d" % (name, index))
    for key, value in values.items():
        if value == "@LUT@":
            value = write_cube()
        elif value == "@LOOK@":
            value = first_look(node)
        if isinstance(value, tuple):
            param(node, key).set(*value)
        else:
            param(node, key).set(value)
    if plugin_id == "net.sf.openfx.TransformPlugin" and "translate" not in values:
        animate(param(node, "translate"), (0.0, 0.0), (14.0, 6.0))
        animate(param(node, "rotate"), (0.0,), (0.8,))
    if name == "reformat":
        # Alternating the target size keeps every copy in a chain resampling.
        param(node, "boxSize").set(*((1920, 1080) if index % 2 == 0 else (3840, 2160)))
    if generator:
        color = "color0" if plugin_id.endswith("CheckerBoardPlugin") or plugin_id == "Noise" else "color"
        animate(param(node, color), (0.1, 0.2, 0.3, 1.0), (0.01, 0.0, 0.0, 0.0))
    return node


def main():
    use_ocio_config()
    app.getProjectParam("outputFormat").set("UHD_4K")
    app.getProjectParam("frameRange").set(1, LAST_FRAME)
    plugin_id, _, generator = NODES[NODE]
    if generator:
        last = configure(NODE, 0)
        count = 1
    else:
        source = make("net.sf.openfx.Ramp", "np_source")
        param(source, "point0").set(0.0, 0.0)
        param(source, "point1").set(3840.0, 2160.0)
        animate(param(source, "color0"), (0.1, 0.2, 0.3, 1.0), (0.01, 0.0, 0.0, 0.0))
        param(source, "color1").set(0.9, 0.8, 0.7, 1.0)
        other = None
        if NODE in TWO_INPUTS:
            other = make("net.sf.openfx.Ramp", "np_source2")
            param(other, "point0").set(3840.0, 0.0)
            param(other, "point1").set(0.0, 2160.0)
            param(other, "color0").set(0.8, 0.1, 0.2, 1.0)
            param(other, "color1").set(0.1, 0.7, 0.9, 1.0)
        size_map = None
        if NODE in MAP_INPUTS:
            size_map = make("net.sf.openfx.Ramp", "np_map")
            param(size_map, "point0").set(0.0, 0.0)
            param(size_map, "point1").set(3840.0, 0.0)
            param(size_map, "color0").set(0.0, 0.0, 0.0, 0.0)
            param(size_map, "color1").set(1.0, 1.0, 1.0, 1.0)
        last = source
        count = CHAIN
        for i in range(count):
            node = configure(NODE, i)
            node.connectInput(0, last)
            if other is not None:
                node.connectInput(1, other)
            if size_map is not None:
                node.connectInput(MAP_INPUTS[NODE], size_map)
            last = node
    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, "np_%s_####.exr" % NODE)
    writer = app.createWriter(path)
    writer.connectInput(0, last)
    comp = writer.getParam("compression")
    if comp is not None:
        comp.set("none")
    print("[node_profile] node=%s plugin=%s chain=%d frames=%d" % (NODE, plugin_id, count, FRAMES))
    for f in range(1, LAST_FRAME + 1):
        app.render(writer, f, f)
    print("[node_profile] done")


main()
