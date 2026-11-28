"""A Natron Utility render reaches the viewer as a device root: no colour download, no CPU fallback.

Graph: USDRead of a generated stage (a camera facing one coloured card) into USDRenderSettings (Default Renderer
Natron Utility, Device GPU) into USDRender into Viewer1. The script sets NATRON_UTILITY_PROFILE (unless it is set
already) before the viewer shows a frame, and reads back the profile's viewer_frame records: one a frame the
viewer shows, with whether it came as a device root, whether that root was drawn from a download instead, the
hand-off rung and the renderer's running count of colour downloads.

Checks, in Ray and then in Raster (where the device cannot rasterize the frame renders as Ray with the node warning
"Render Mode is Raster, but ..."; that is noted, not failed):
  1. the viewer shows a frame of the USDRender, and EDITS more after as many Seed changes;
  2. the viewer's hand-off rung is not cpu;
  3. every one of those frames came to the viewer as a device root (device_color);
  4. none of them was drawn from a download of the root (no legacy fallback);
  5. the renderer's colour download count did not move from the first frame to the last.

Needs a GPU build and a viewer with a device hand-off: run with NATRON_GPU=force and, under Xvfb, a rung that
lavapipe can take, e.g. (natron-dev container, repository root):
    docker exec -e NATRON_GPU=force -e NATRON_VIEWER_HANDOFF=hostsync -w /home/bosley/git/Natron natron-dev \\
        Tests/gui/run-gui-test.sh Tests/gui/m514_utility_device_viewer.py build/release/App/Natron
Screenshots and results.txt go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every
check passed.
"""
import json
import os
import sys
import traceback

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(ROOT, "Tests", "gui"))

POLL_TIMEOUT_S = 60.0
SETTLE_MS = 700
UTILITY_ID = "HdNatronUtilityRendererPlugin"
FALLBACK_TEXT = "Render Mode is Raster, but"
SIZE = (480, 270)
EDITS = 3

CARD_STAGE = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Camera "Cam"
    {
        float focalLength = 24
        float horizontalAperture = 36
        float verticalAperture = 20.25
        float2 clippingRange = (0.1, 200)
        double3 xformOp:translate = (0, 0, 4)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Mesh "Card"
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, -1, 0), (2, -1, 0), (2, 1, 0), (-2, 1, 0)]
        color3f[] primvars:displayColor = [(0.9, 0.35, 0.1)]
    }
}
"""

try:
    import guitest
    from guitest import OUT, check, plugin_problem, report, require, run, section, shot

    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    # The renderer reads the variable once, at the first frame it profiles, which is after this.
    if not os.environ.get("NATRON_UTILITY_PROFILE"):
        os.environ["NATRON_UTILITY_PROFILE"] = os.path.join(OUT, "m514-utility-device-viewer-profile.jsonl")
    PROFILE = os.environ["NATRON_UTILITY_PROFILE"]
    if os.path.exists(PROFILE):
        os.remove(PROFILE)
    stage_path = os.path.join(OUT, "m514-device-card.usda")
    with open(stage_path, "w") as f:
        f.write(CARD_STAGE)

    read = app.createNode("USDRead")
    require(read is not None, "create USDRead", plugin_problem("USDRead"))
    settings = app.createNode("USDRenderSettings")
    require(settings is not None, "create USDRenderSettings", plugin_problem("USDRenderSettings"))
    render = app.createNode("USDRender")
    require(render is not None, "create USDRender", plugin_problem("USDRender"))
    viewer = app.getNode("Viewer1") or app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    read.setPosition(0, 0)
    settings.setPosition(0, 100)
    render.setPosition(0, 200)
    viewer.setPosition(0, 300)
    viewer_name = str(viewer.getScriptName())

    def options(param):
        return [str(o) for o in param.getOptions()]

    def choose(param, *wanted):
        lowered = [i.lower() for i in options(param)]
        for w in wanted:
            if w.lower() in lowered:
                param.set(lowered.index(w.lower()))
                return True
        return False

    def utility_knob(suffix):
        for p in settings.getParams():
            name = str(p.getScriptName())
            if name.lower().startswith("natronutility") and name.endswith(suffix):
                return p
        return None

    def viewer_frames():
        """The profile's viewer_frame records of Viewer1, oldest first."""
        out = []
        try:
            with open(PROFILE) as f:
                for line in f:
                    try:
                        record = json.loads(line)
                    except ValueError:
                        continue
                    if record.get("record") == "viewer_frame" and record.get("viewer") == viewer_name:
                        out.append(record)
        except (IOError, OSError):
            pass
        return out

    def frames_shown(after):
        """Steps that wait for a viewer frame past the first `after`; returns whether one came."""
        ok = yield ("until", lambda: len(viewer_frames()) > after, POLL_TIMEOUT_S)
        if ok:
            yield ("sleep", SETTLE_MS)
        return ok

    def check_frames(name, frames):
        require(bool(frames), "%s: the viewer showed frames of the USDRender" % name)
        rungs = sorted(set(str(f.get("rung")) for f in frames))
        report("NOTE %s: %d viewer frames, rungs %s, device colour %d, fallback %d, colour downloads %s -> %s" % (
            name, len(frames), ",".join(rungs), sum(1 for f in frames if f.get("device_color")),
            sum(1 for f in frames if f.get("fallback")), frames[0].get("device_downloads"), frames[-1].get("device_downloads")))
        check("Cpu" not in rungs, "%s: the viewer's hand-off rung is not cpu (%s); run with NATRON_GPU=force" % (name, ",".join(rungs)))
        check(all(f.get("device_color") for f in frames),
              "%s: every frame reached the viewer as a device root (%d of %d)" % (
                  name, sum(1 for f in frames if f.get("device_color")), len(frames)))
        check(not any(f.get("fallback") for f in frames),
              "%s: no frame was drawn from a download of its device root (%d did)" % (
                  name, sum(1 for f in frames if f.get("fallback"))))
        check(frames[-1].get("device_downloads") == frames[0].get("device_downloads"),
              "%s: the renderer made no colour download (%s -> %s)" % (
                  name, frames[0].get("device_downloads"), frames[-1].get("device_downloads")))

    def session(name, seed):
        """Steps of one mode: a frame, then EDITS Seed changes, each awaited; checks the frames they brought."""
        section(name)
        before = len(viewer_frames())
        seed.set(seed.get() + 1)
        ok = yield from frames_shown(before)
        require(ok, "%s: the viewer shows a frame of the USDRender within %g s" % (name, POLL_TIMEOUT_S))
        first = len(viewer_frames()) - 1
        for i in range(EDITS):
            count = len(viewer_frames())
            seed.set(seed.get() + 1)
            ok = yield from frames_shown(count)
            check(ok, "%s: edit %d brings a new viewer frame" % (name, i + 1))
        message = str(render.getPersistentMessage() or "")
        if FALLBACK_TEXT in message:
            report("NOTE %s rendered as Ray on this device: %r" % (name, message))
        shot("m514-utility-device-viewer-%s.png" % name)
        check_frames(name, viewer_frames()[first:])

    def main():
        yield ("sleep", SETTLE_MS)
        section("setup")
        guitest.close_all_panels()
        require(settings.connectInput(0, read), "connect the USDRenderSettings to the USDRead")
        require(render.connectInput(0, settings), "connect the USDRender to the USDRenderSettings")
        read.getParam("filename").setValue(stage_path)
        ok = yield ("until", lambda: "/World/Cam" in options(settings.getParam("camera")), POLL_TIMEOUT_S)
        require(ok, "the settings' Camera menu lists /World/Cam")
        choose(settings.getParam("camera"), "/World/Cam")
        require(choose(settings.getParam("defaultRenderer"), UTILITY_ID), "the settings' Default Renderer lists Natron Utility")
        settings.getParam("NatronParamFormatSize").set(SIZE[0], SIZE[1])
        device = utility_knob("Device")
        require(device is not None, "the Natron Utility settings have a Device parameter")
        require(choose(device, "GPU"), "the Device menu lists GPU (%r)" % options(device))
        mode = utility_knob("RenderMode")
        require(mode is not None, "the Natron Utility settings have a Render Mode parameter")
        seed = utility_knob("Seed")
        require(seed is not None, "the Natron Utility settings have a Seed parameter")
        pixel_samples = utility_knob("PixelSamples")
        if pixel_samples is not None:
            pixel_samples.set(1)
        require(choose(mode, "Ray"), "Render Mode is Ray")
        require(viewer.connectInput(0, render), "connect the Viewer to the USDRender")

        yield from session("ray", seed)
        require(choose(mode, "Raster"), "Render Mode switches to Raster")
        yield from session("raster", seed)
        viewer.disconnectInput(0)

    run(main())
except Exception:
    from guitest import check as _check, finish as _finish
    _check(False, "script error: " + traceback.format_exc())
    _finish()
