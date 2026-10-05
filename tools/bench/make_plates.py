# Writes the synthetic plates the plate-fed graph_bench.py families read. Run inside NatronRenderer:
#
#   BENCH_PLATES_RES=all NatronRenderer -b tools/bench/make_plates.py
#
# For each resolution, three 8-frame half-float RGBA EXR sequences plate_<res>_<1..3>.####.exr
# (plates 1 and 2 ZIP, plate 3 PIZ), and one HD deep sequence deep_hd.####.exr (DeepFromImage of
# plate 1's image with a Z ramp from 1 to 100 across the frame). The images are checkerboards with
# full-frame noise reseeded every frame, so they compress about as badly as grainy footage and no
# two frames are alike. A sequence whose frames all exist is skipped.
#
# Environment:
#   BENCH_PLATES_RES     hd | uhd | all (default all); the deep sequence is made with hd
#   BENCH_PLATES_DIR     output directory (default build/bench/fixtures)
#   BENCH_PLATE_FRAMES   frames per sequence (default 8)

import glob
import os
import sys
import time

PLATES_RES = os.environ.get("BENCH_PLATES_RES", "all")
PLATES = os.environ.get("BENCH_PLATES_DIR") or os.path.join(os.getcwd(), "build", "bench", "fixtures")
FRAMES = int(os.environ.get("BENCH_PLATE_FRAMES", "8"))

FORMATS = {"hd": ("HD", 1920, 1080), "uhd": ("UHD_4K", 3840, 2160)}
COMPRESSION = {1: "zip", 2: "zip", 3: "piz"}


def log(msg):
    print("[plates] " + msg)
    sys.stdout.flush()


def make(plugin_id):
    node = app.createNode(plugin_id)
    if node is None:
        raise RuntimeError("could not create " + plugin_id)
    return node


def param(node, name):
    p = node.getParam(name)
    if p is None:
        raise RuntimeError("%s has no param %s" % (node.getPluginID(), name))
    return p


def set_choice(node, name, value):
    p = param(node, name)
    p.set(value)
    shown = p.getOption(p.get())
    if shown != value:
        raise RuntimeError("%s.%s is %s after setting %s" % (node.getPluginID(), name, shown, value))


def existing(stem):
    files = [os.path.join(PLATES, "%s.%04d.exr" % (stem, f)) for f in range(1, FRAMES + 1)]
    return files, all(os.path.isfile(p) and os.path.getsize(p) > 0 for p in files)


def report(stem, files, seconds):
    sizes = [os.path.getsize(p) for p in files]
    log("%s: %d frames, %.1f MB total, %.2f MB per frame, %.1fs"
        % (stem, len(sizes), sum(sizes) / 1e6, sum(sizes) / 1e6 / len(sizes), seconds))


def image(index):
    cb = make("net.sf.openfx.CheckerBoardPlugin")
    set_choice(cb, "extent", "project")
    color = param(cb, "color0")
    for f in range(1, FRAMES + 1):
        color.setValueAtTime(0.1 + 0.02 * f + 0.05 * index, f, 0)
    noise = make("net.sf.openfx.Noise")
    set_choice(noise, "extent", "project")
    param(noise, "seed").set(1000 * index)
    m = make("net.sf.openfx.MergePlugin")
    m.connectInput(0, cb)
    m.connectInput(1, noise)
    param(m, "mix").set(0.5)
    return m


def render(writer, stem, files):
    t0 = time.time()
    app.render(writer, 1, FRAMES)
    missing = [p for p in files if not os.path.isfile(p)]
    if missing:
        raise RuntimeError("%s: render left %d frames missing, first %s" % (stem, len(missing), missing[0]))
    report(stem, files, time.time() - t0)


def plates(res):
    fmt, width, height = FORMATS[res]
    app.getProjectParam("outputFormat").set(fmt)
    app.getProjectParam("frameRange").set(1, FRAMES)
    first = None
    for index in (1, 2, 3):
        stem = "plate_%s_%d" % (res, index)
        files, done = existing(stem)
        if done:
            report(stem, files, 0.0)
            continue
        src = image(index)
        if first is None:
            first = src
        writer = app.createWriter(os.path.join(PLATES, stem + ".####.exr"))
        writer.connectInput(0, src)
        set_choice(writer, "compression", COMPRESSION[index])
        set_choice(writer, "bitDepth", "16f")
        render(writer, stem, files)
    return width, height


def deep(width, height):
    stem = "deep_hd"
    files, done = existing(stem)
    if done:
        report(stem, files, 0.0)
        return
    app.getProjectParam("outputFormat").set("HD")
    src = image(1)
    z = make("net.sf.openfx.Ramp")
    z.connectInput(0, src)
    param(z, "point0").set(0.0, 0.0)
    param(z, "point1").set(float(width), float(height))
    param(z, "color0").set(1.0, 1.0, 1.0, 1.0)
    param(z, "color1").set(100.0, 100.0, 100.0, 1.0)
    d = make("fr.natron.DeepFromImage")
    d.connectInput(0, src)
    d.connectInput(1, z)
    w = make("fr.natron.DeepWrite")
    param(w, "filename").set(os.path.join(PLATES, stem + ".####.exr"))
    w.connectInput(0, d)
    render(w, stem, files)


def main():
    os.makedirs(PLATES, exist_ok=True)
    wanted = ("hd", "uhd") if PLATES_RES == "all" else (PLATES_RES,)
    for res in wanted:
        if res not in FORMATS:
            raise RuntimeError("BENCH_PLATES_RES must be hd, uhd or all, not %s" % PLATES_RES)
    for res in wanted:
        plates(res)
    if "hd" in wanted:
        deep(*FORMATS["hd"][1:])
    total = sum(os.path.getsize(p) for p in glob.glob(os.path.join(PLATES, "*.exr")))
    log("%s holds %.1f MB of plates" % (PLATES, total / 1e6))


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    sys.stdout.flush()
    os._exit(3)
os._exit(0)
