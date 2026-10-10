# Builds one of the three synthetic UHD float comps (COMP=keying_grade|cg_multipass|defocus_retime) and saves it as .ntp.
# Run inside NatronRenderer:
#
#   COMP=keying_grade COMPS_OUT=tools/bench/comps NatronRenderer -b tools/bench/comps/build_comps.py
#
# The comps have no external inputs: plates are generators animated per frame, so no frame is
# served from another frame's cache. Writers target build/bench/m82-profile/out under the working
# directory, and the project frame range is 1-8.

import os
import re

import NatronEngine

OUT = os.environ.get("COMPS_OUT", os.path.join(os.getcwd(), "tools", "bench", "comps"))
RENDER_DIR = os.path.join(os.getcwd(), "build", "bench", "m82-profile", "out")
# [Project] expands to the directory holding the .ntp, so the saved comps carry no host paths.
WRITE_PATTERN = "[Project]/../../../build/bench/m82-profile/out/%s.####.exr"
LAST_FRAME = 8


def make(plugin_id, name):
    props = {"CreateNodeArgsPropNodeInitialName": NatronEngine.StringNodeCreationProperty(name)}
    node = app.createNode(plugin_id, -1, None, props)
    if node is None:
        raise RuntimeError("could not create " + plugin_id)
    return node


def p(node, name):
    prm = node.getParam(name)
    if prm is None:
        raise RuntimeError("%s has no param %s" % (node.getPluginID(), name))
    return prm


def choice(node, name, value):
    prm = p(node, name)
    prm.set(value)
    shown = prm.getOption(prm.get())
    if str(shown).lower() != value.lower():
        raise RuntimeError("%s.%s is %s after setting %s" % (node.getScriptName(), name, shown, value))


def animate(prm, values_by_frame, dim=0):
    for f, v in values_by_frame:
        prm.setValueAtTime(v, f, dim)


def drift(prm, base, per_frame, dims=1):
    for d in range(dims):
        animate(prm, [(f, base[d] + per_frame[d] * f) for f in range(1, LAST_FRAME + 1)], d)


def wire(node, *inputs):
    for i, src in enumerate(inputs):
        if src is not None:
            node.connectInput(i, src)
    return node


def single(plugin_id, name, src):
    return wire(make(plugin_id, name), src)


def grade(name, src, mult=(1.0, 1.0, 1.0, 1.0), gamma=(1.0, 1.0, 1.0, 1.0), offset=(0.0, 0.0, 0.0, 0.0)):
    n = single("net.sf.openfx.GradePlugin", name, src)
    p(n, "multiply").set(*mult)
    p(n, "gamma").set(*gamma)
    p(n, "offset").set(*offset)
    return n


def colorcorrect(name, src, sat=1.0, gain=1.0, gamma=1.0):
    n = single("net.sf.openfx.ColorCorrectPlugin", name, src)
    p(n, "MasterSaturation").set(sat, sat, sat, 1.0)
    p(n, "MasterGain").set(gain, gain, gain, 1.0)
    p(n, "MasterGamma").set(gamma, gamma, gamma, 1.0)
    return n


def blur(name, src, size, mask=None):
    n = single("net.sf.cimg.CImgBlur", name, src)
    p(n, "size").set(size, size)
    if mask is not None:
        n.connectInput(1, mask)
        p(n, "enableMask_Mask").set(True)
    return n


def merge(name, op, a, b):
    n = make("net.sf.openfx.MergePlugin", name)
    n.connectInput(0, b)
    n.connectInput(1, a)
    choice(n, "operation", op)
    return n


def ramp(name, c0, c1):
    n = make("net.sf.openfx.Ramp", name)
    p(n, "point0").set(0.0, 0.0)
    p(n, "point1").set(3840.0, 2160.0)
    p(n, "color0").set(*c0)
    p(n, "color1").set(*c1)
    return n


def moving_ramp(name, c0, c1, speed=0.01):
    n = ramp(name, c0, c1)
    prm = p(n, "color0")
    for d in range(3):
        animate(prm, [(f, c0[d] + speed * f) for f in range(1, LAST_FRAME + 1)], d)
    return n


def rectangle(name, color, x, y, w, h, soft=0.2, dx=0.0):
    n = make("net.sf.openfx.Rectangle", name)
    p(n, "color0").set(*color)
    p(n, "softness").set(soft)
    p(n, "size").set(w, h)
    drift(p(n, "bottomLeft"), (x, y), (dx, 0.0), 2)
    return n


def radial(name, color, speed=0.01):
    n = make("net.sf.openfx.Radial", name)
    p(n, "color0").set(*color)
    prm = p(n, "color0")
    for d in range(3):
        animate(prm, [(f, color[d] + speed * f) for f in range(1, LAST_FRAME + 1)], d)
    return n


def checker(name, boxes=40):
    n = make("net.sf.openfx.CheckerBoardPlugin", name)
    p(n, "boxSize").set(3840.0 / boxes, 3840.0 / boxes)
    animate(p(n, "color0"), [(f, 0.05 + 0.01 * f) for f in range(1, LAST_FRAME + 1)], 0)
    return n


def finish(comp, root):
    os.makedirs(RENDER_DIR, exist_ok=True)
    props = {"CreateNodeArgsPropNodeInitialName": NatronEngine.StringNodeCreationProperty("Write")}
    writer = app.createWriter(WRITE_PATTERN % comp, None, props)
    writer.connectInput(0, root)
    comp_param = writer.getParam("compression")
    if comp_param is not None:
        comp_param.set("none")
    bit_depth = writer.getParam("bitDepth")
    if bit_depth is not None:
        try:
            choice(writer, "bitDepth", "32f")
        except RuntimeError:
            pass
    path = os.path.join(OUT, comp + ".ntp")
    app.saveProject(path)
    # The saved Project variable is the directory at save time; Natron resets it on load.
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    text = text.replace(os.path.abspath(OUT), "")
    text = re.sub(r"<Value>[^<]*\.####\.exr</Value>", "<Value>%s</Value>" % (WRITE_PATTERN % comp), text)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    for leftover in (path + ".lock", path + ".~1~"):
        if os.path.exists(leftover):
            os.remove(leftover)


def keying_grade():
    bg_ramp = moving_ramp("plate_ramp", (0.05, 0.55, 0.12, 1.0), (0.1, 0.75, 0.2, 1.0))
    subject = rectangle("plate_subject", (0.45, 0.28, 0.2, 1.0), 1100, 200, 1500, 1800, 0.25, 6.0)
    hair = rectangle("plate_hair", (0.2, 0.12, 0.08, 1.0), 1500, 1200, 700, 900, 0.5, -4.0)
    plate = merge("plate", "over", hair, merge("plate_sub", "over", subject, bg_ramp))

    key = single("net.sf.openfx.KeyerPlugin", "keyer", plate)
    p(key, "keyColor").set(0.07, 0.65, 0.16, 1.0)
    choice(key, "mode", "screen")
    chroma = single("net.sf.openfx.ChromaKeyerPlugin", "chromakey", plate)
    p(chroma, "keyColor").set(0.07, 0.65, 0.16, 1.0)
    core = merge("matte_max", "max", chroma, key)
    eroded = single("net.sf.cimg.CImgErode", "matte_erode", core)
    p(eroded, "size").set(6, 6)
    soft = blur("matte_blur", eroded, 10.0)
    grown = single("net.sf.cimg.CImgDilate", "matte_dilate", soft)
    p(grown, "size").set(2, 2)

    clean = single("net.sf.openfx.Despill", "despill", plate)
    fg = merge("fg_in_matte", "in", clean, grown)
    fg = grade("fg_grade1", fg, mult=(1.05, 1.0, 0.95, 1.0))
    fg = colorcorrect("fg_cc1", fg, sat=1.1, gain=1.05)
    fg = grade("fg_grade2", fg, gamma=(1.02, 1.0, 0.98, 1.0), offset=(0.01, 0.0, -0.01, 0.0))
    fg = colorcorrect("fg_cc2", fg, sat=0.95, gamma=1.05)

    sky = moving_ramp("bg_ramp", (0.3, 0.4, 0.7, 1.0), (0.8, 0.7, 0.6, 1.0), 0.005)
    sky = grade("bg_grade", sky, mult=(0.9, 0.95, 1.05, 1.0))
    sky = blur("bg_blur", sky, 24.0)

    comp = merge("comp_over", "over", fg, sky)
    comp = colorcorrect("final_cc", comp, sat=1.05)
    comp = grade("final_grade", comp, mult=(1.02, 1.0, 0.98, 1.0))
    finish("keying_grade", comp)


def cg_multipass():
    gens = [
        lambda n: radial(n, (0.35, 0.3, 0.25, 1.0)),
        lambda n: moving_ramp(n, (0.0, 0.0, 0.0, 1.0), (0.4, 0.4, 0.4, 1.0)),
        lambda n: checker(n),
        lambda n: rectangle(n, (0.2, 0.25, 0.3, 1.0), 400, 300, 3000, 1500, 0.3, 4.0),
        lambda n: radial(n, (0.1, 0.12, 0.2, 1.0), 0.02),
        lambda n: moving_ramp(n, (0.0, 0.0, 0.0, 1.0), (0.2, 0.15, 0.1, 1.0)),
        lambda n: rectangle(n, (0.8, 0.8, 0.8, 1.0), 200, 100, 3400, 1900, 0.4, -3.0),
        lambda n: moving_ramp(n, (1.0, 1.0, 1.0, 1.0), (0.0, 0.0, 0.0, 1.0), 0.0),
    ]
    names = ("diffuse", "spec", "refl", "sss", "gi", "emission", "ao", "depth")
    passes = {}
    for i, name in enumerate(names):
        src = gens[i](name + "_src")
        passes[name] = grade(name + "_grade", src, mult=(1.0 + 0.03 * i, 1.0, 1.0 - 0.02 * i, 1.0))

    beauty = passes["diffuse"]
    for name in ("spec", "refl", "sss", "gi"):
        beauty = merge("add_" + name, "plus", passes[name], beauty)
    beauty = merge("mult_ao", "multiply", passes["ao"], beauty)
    beauty = merge("add_emission", "plus", passes["emission"], beauty)

    glow_src = grade("glow_thresh", beauty, offset=(-0.4, -0.4, -0.4, 0.0))
    glow = blur("glow_blur", glow_src, 90.0)
    glow = grade("glow_grade", glow, mult=(1.4, 1.3, 1.1, 1.0))
    lit = merge("glow_screen", "screen", glow, beauty)

    depth = grade("depth_remap", passes["depth"], mult=(1.5, 1.5, 1.5, 1.0))
    defocused = blur("defocus_blur", lit, 60.0, mask=depth)
    out = colorcorrect("final_cc", defocused, sat=1.08)
    out = grade("final_grade", out, mult=(1.03, 1.0, 0.97, 1.0))
    finish("cg_multipass", out)


def defocus_retime():
    base = moving_ramp("plate_ramp", (0.1, 0.12, 0.2, 1.0), (0.6, 0.5, 0.4, 1.0))
    boxes = checker("plate_checker", 24)
    subject = rectangle("plate_subject", (0.7, 0.5, 0.3, 1.0), 900, 400, 1700, 1300, 0.2, 8.0)
    plate = merge("plate_sub", "over", subject, merge("plate_boxes", "over", boxes, base))
    plate = grade("plate_grade", plate, mult=(1.1, 1.0, 0.9, 1.0))
    plate = blur("plate_soften", plate, 12.0)

    tb = single("net.sf.openfx.TimeBlur", "timeblur", plate)
    p(tb, "division").set(5)
    p(tb, "shutter").set(1.0)
    rt = single("net.sf.openfx.Retime", "retime", tb)
    p(rt, "speed").set(0.5)
    fb = single("net.sf.openfx.FrameBlend", "frameblend", rt)
    p(fb, "frameRange").set(-2, 2)

    far = blur("defocus_far", fb, 300.0)
    far = grade("defocus_far_grade", far, mult=(0.9, 0.95, 1.0, 1.0))
    mid = blur("defocus_mid", rt, 100.0)
    mid = colorcorrect("defocus_mid_cc", mid, sat=1.1)

    xf = single("net.sf.openfx.TransformPlugin", "xform_mb", mid)
    drift(p(xf, "translate"), (0.0, 0.0), (14.0, 6.0), 2)
    animate(p(xf, "rotate"), [(f, 0.8 * f) for f in range(1, LAST_FRAME + 1)])
    p(xf, "center").set(1920.0, 1080.0)
    p(xf, "motionBlur").set(1.0)
    p(xf, "shutter").set(1.0)

    comp = merge("comp_over", "over", xf, far)
    comp = colorcorrect("final_cc", comp, sat=1.05)
    comp = grade("final_grade", comp, mult=(1.02, 1.0, 0.98, 1.0))
    finish("defocus_retime", comp)


BUILDERS = {"keying_grade": keying_grade, "cg_multipass": cg_multipass, "defocus_retime": defocus_retime}
app.getProjectParam("outputFormat").set("UHD_4K")
app.getProjectParam("frameRange").set(1, LAST_FRAME)
BUILDERS[os.environ["COMP"]]()
# A script run with -b renders the project's writers when it ends; the exit skips that.
os._exit(0)
