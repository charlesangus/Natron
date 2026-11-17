"""Natron Utility projection modes (M288 - Utility Renderer Projection Modes): spherical and cylindrical
camera projections, overscan, and the UV Unwrap bake.

Graph: USDRead of a generated stage into USDRenderSettings (Default Renderer Natron Utility) into USDRender,
with Viewer1 on the USDRender. The settings node's Natron Utility tab is driven through its Projection,
Overscan, UV Prim, UV Primvar, UDIM Tile and Pixel Samples parameters (found by suffix, as the page prefix
is derived from the renderer id).

Stage "Ring": a camera at the origin looking down -Z, six coloured cubes on a ring of radius 6 around it
(red, orange, yellow, green, cyan, magenta), a grey ground plane, a blue sphere overhead, a distant light
and a dome light. Stage "UvCube": one cube whose six faces have distinct colours (displayColor, one per
face) laid out in a cross in UV space, and a distant light.

Each case is rendered through a Write to a PNG in the output directory (<name>-render.png), which is
checked, and the viewer is then shot (<name>.png):
  1. m288-spherical:   Ring, Projection Spherical, 1024x512 format (2:1); the whole surround is in frame.
  2. m288-cylindrical: Ring, Projection Cylindrical, same format.
  3. m288-overscan:    Ring, Projection Camera, 960x540 format, Overscan 96 px: the render is 192 px wider
                       and taller than the format.
  4. m288-uv-unwrap:   UvCube, Projection UV Unwrap, UV Prim /World/Cube, 512x512 format: the cube's
                       shaded faces fill their cross in the 1001 tile and the rest of the tile stays empty.
Each render must write its file (no node error), and the image must not be empty: at least 5% of its
pixels coloured, in at least 4 distinct colours.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import math
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 60.0
SETTLE_MS = 700
VIEWER_SETTLE_MS = 2500
STARTUP_TIMEOUT_S = 60.0
UTILITY_ID = "HdNatronUtilityRendererPlugin"
SAMPLES = 4
OVERSCAN_PX = 96
MIN_COVERAGE = 0.05
MIN_COLOURS = 4
LATLONG = (1024, 512)
PERSPECTIVE = (960, 540)
UV_TILE = (512, 512)

RING_COLOURS = [(0.9, 0.1, 0.1), (0.95, 0.5, 0.05), (0.95, 0.9, 0.1), (0.1, 0.8, 0.2), (0.1, 0.8, 0.9), (0.85, 0.1, 0.85)]


def cube_prim(name, size, colour, translate):
    return ('    def Cube "%s"\n    {\n        double size = %g\n        double3 xformOp:translate = (%g, %g, %g)\n'
            '        uniform token[] xformOpOrder = ["xformOp:translate"]\n'
            '        color3f[] primvars:displayColor = [(%g, %g, %g)]\n    }\n\n' % ((name, size) + translate + colour))


def ring_stage():
    text = '#usda 1.0\n(\n    defaultPrim = "World"\n    upAxis = "Y"\n)\n\ndef Xform "World"\n{\n'
    text += ('    def Camera "Cam"\n    {\n        float focalLength = 24\n        float horizontalAperture = 36\n'
             '        float verticalAperture = 20.25\n        float2 clippingRange = (0.1, 200)\n'
             '        double3 xformOp:translate = (0, 0, 0)\n        uniform token[] xformOpOrder = ["xformOp:translate"]\n    }\n\n')
    for i, colour in enumerate(RING_COLOURS):
        angle = math.radians(i * 60.0)
        text += cube_prim("Cube%d" % i, 1.6, colour, (6.0 * math.sin(angle), 0.0, -6.0 * math.cos(angle)))
    text += cube_prim("Ground", 1.0, (0.45, 0.45, 0.45), (0.0, -2.0, 0.0)).replace(
        "double size = 1", "double size = 1\n        double3 xformOp:scale = (40, 0.1, 40)").replace(
        'xformOpOrder = ["xformOp:translate"]', 'xformOpOrder = ["xformOp:translate", "xformOp:scale"]')
    text += ('    def Sphere "Sky"\n    {\n        double radius = 1.2\n        double3 xformOp:translate = (0, 6, -3)\n'
             '        uniform token[] xformOpOrder = ["xformOp:translate"]\n'
             '        color3f[] primvars:displayColor = [(0.2, 0.3, 0.95)]\n    }\n\n')
    text += '    def DistantLight "Key"\n    {\n        float inputs:intensity = 3\n        float inputs:angle = 0\n'
    text += '        float3 xformOp:rotateXYZ = (-50, 30, 0)\n        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]\n    }\n\n'
    text += '    def DomeLight "Dome"\n    {\n        float inputs:intensity = 0.6\n        color3f inputs:color = (0.6, 0.75, 1.0)\n    }\n}\n'
    return text


UV_FACES = [
    # (name, point indices, cell column, cell row, colour)
    ("front", (4, 5, 6, 7), 1, 1, (0.9, 0.15, 0.15)),
    ("back", (1, 0, 3, 2), 3, 1, (0.15, 0.8, 0.2)),
    ("right", (5, 1, 2, 6), 2, 1, (0.15, 0.3, 0.95)),
    ("left", (0, 4, 7, 3), 0, 1, (0.95, 0.85, 0.1)),
    ("top", (7, 6, 2, 3), 1, 2, (0.9, 0.2, 0.9)),
    ("bottom", (0, 1, 5, 4), 1, 0, (0.1, 0.85, 0.9)),
]


def uv_stage():
    points = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1), (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
    indices = []
    uvs = []
    for _name, face, col, row, _colour in UV_FACES:
        indices.extend(face)
        u0, u1, v0, v1 = col / 4.0, (col + 1) / 4.0, row / 3.0, (row + 1) / 3.0
        uvs.extend([(u0, v0), (u1, v0), (u1, v1), (u0, v1)])
    return """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Cube"
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [%s]
        point3f[] points = [%s]
        texCoord2f[] primvars:st = [%s] (
            interpolation = "faceVarying"
        )
        color3f[] primvars:displayColor = [%s] (
            interpolation = "uniform"
        )
    }

    def DistantLight "Key"
    {
        float inputs:intensity = 3
        float inputs:angle = 0
        float3 xformOp:rotateXYZ = (-40, 35, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }

    def DomeLight "Fill"
    {
        float inputs:intensity = 0.4
    }
}
""" % (", ".join(str(i) for i in indices), ", ".join("(%g, %g, %g)" % p for p in points),
       ", ".join("(%g, %g)" % uv for uv in uvs), ", ".join("(%g, %g, %g)" % f[4] for f in UV_FACES))


try:
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QImage
    from PySide6.QtTest import QTest

    from guitest import OUT, check, finish, plugin_problem, report, require, run, section, shot, viewer_up, widgets_of

    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    ring_path = os.path.join(OUT, "m288-ring.usda")
    uv_path = os.path.join(OUT, "m288-uvcube.usda")
    with open(ring_path, "w") as f:
        f.write(ring_stage())
    with open(uv_path, "w") as f:
        f.write(uv_stage())

    read = app.createNode("USDRead")
    require(read is not None, "create USDRead", plugin_problem("USDRead"))
    settings = app.createNode("USDRenderSettings")
    require(settings is not None, "create USDRenderSettings", plugin_problem("USDRenderSettings"))
    render = app.createNode("USDRender")
    require(render is not None, "create USDRender", plugin_problem("USDRender"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    read.setPosition(0, 0)
    settings.setPosition(0, 100)
    render.setPosition(0, 200)
    viewer.setPosition(200, 300)

    def options(param):
        return [str(o) for o in param.getOptions()]

    def choose(param, *wanted):
        """Sets a choice parameter to the entry whose id or label matches one of wanted, ignoring case."""
        ids = options(param)
        lowered = [i.lower() for i in ids]
        for w in wanted:
            if w.lower() in lowered:
                param.set(lowered.index(w.lower()))
                return True
        return False

    def utility_knob(suffix):
        """The settings node's Natron Utility parameter whose script name ends in suffix."""
        for p in settings.getParams():
            name = str(p.getScriptName())
            if name.lower().startswith("natronutility") and name.endswith(suffix):
                return p
        return None

    def set_format(label, size):
        """Puts the project format and the USDRenderSettings prim's resolution on size.

        USDRender takes its resolution from the active RenderSettings prim, which the settings node's
        hidden Size knob authors, so that knob is the one that matters; the project format and the
        settings' Format menu are moved too. A size the project already lists under another name is
        not added twice, so the menus are matched by size as well as by label.
        """
        app.addFormat("%s %dx%d" % (label, size[0], size[1]))
        wanted = "%dx%d" % (size[0], size[1])
        for fmt in (app.getProjectParam("outputFormat"), settings.getParam("NatronParamFormatChoice")):
            if fmt is None:
                continue
            ids = [str(o) for o in fmt.getOptions()]
            index = next((i for i, o in enumerate(ids) if label in o), None)
            if index is None:
                index = next((i for i, o in enumerate(ids) if wanted in o), None)
            if index is not None:
                fmt.setValue(index)
        dims = settings.getParam("NatronParamFormatSize")
        require(dims is not None, "the settings node has a format Size parameter")
        dims.set(size[0], size[1])
        report("NOTE %s: settings resolution now %s" % (label, [dims.get()[0], dims.get()[1]] if isinstance(dims.get(), (list, tuple)) else dims.get()))

    def analyse(path):
        """(width, height, coloured share, distinct colours) of the PNG at path."""
        image = QImage(path)
        if image.isNull():
            return None
        coloured = 0
        total = 0
        colours = set()
        for y in range(0, image.height(), 2):
            for x in range(0, image.width(), 2):
                c = image.pixelColor(x, y)
                total += 1
                if c.alpha() > 8 and max(c.red(), c.green(), c.blue()) > 8:
                    coloured += 1
                    colours.add((c.red() >> 5, c.green() >> 5, c.blue() >> 5))
        return image.width(), image.height(), (coloured / float(total) if total else 0.0), len(colours)

    writer_state = {}

    def render_to_png(name):
        """Renders the USDRender through a Write to OUT/<name>-render.png; returns its analysis or None."""
        path = os.path.join(OUT, name + "-render.png")
        if os.path.exists(path):
            os.remove(path)
        if "writer" not in writer_state:
            writer_state["writer"] = app.createWriter(path)
            require(writer_state["writer"] is not None, "create a Write")
            writer_state["writer"].setPosition(200, 200)
            require(writer_state["writer"].connectInput(0, render), "connect the Write to the USDRender")
        writer = writer_state["writer"]
        writer.getParam("filename").setValue(path)
        app.renderBlocking(writer, 1, 1)
        report("NOTE render %s -> %s" % (name, path))
        if not os.path.isfile(path):
            return None
        report("SHOT " + path)
        return analyse(path)

    def check_image(name, result):
        check(result is not None, "%s: the render wrote its image (no node error)" % name)
        if result is None:
            return
        width, height, share, colours = result
        report("NOTE %s render is %dx%d, %.1f%% coloured, %d distinct colours" % (name, width, height, share * 100.0, colours))
        check(share >= MIN_COVERAGE, "%s: the image is not empty (%.1f%% of pixels coloured)" % (name, share * 100.0))
        check(colours >= MIN_COLOURS, "%s: the image has several colours (%d)" % (name, colours))

    def show_and_shoot(name):
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, render), "connect the Viewer to the USDRender")
        yield ("until", viewer_up, STARTUP_TIMEOUT_S)
        yield ("sleep", VIEWER_SETTLE_MS)
        for w in widgets_of("ViewerGL"):
            if w.isVisible():
                QTest.keyClick(w, Qt.Key_F)
                break
        yield ("sleep", SETTLE_MS)
        shot(name + ".png")

    def setup_case(stage, projection, overscan=0, uv_prim=""):
        read.getParam("filename").setValue(stage)
        require(choose(settings.getParam("defaultRenderer"), UTILITY_ID), "the settings' Default Renderer lists Natron Utility")
        proj = utility_knob("Projection")
        require(proj is not None, "the Natron Utility tab has a Projection parameter")
        check(choose(proj, projection), "Projection offers %s (%r)" % (projection, options(proj)))
        over = utility_knob("Overscan")
        require(over is not None, "the Natron Utility tab has an Overscan parameter")
        over.set(overscan)
        samples = utility_knob("PixelSamples")
        if samples is not None:
            samples.set(SAMPLES)
        else:
            report("NOTE no Pixel Samples parameter found")
        if uv_prim:
            prim = utility_knob("UvPrim")
            require(prim is not None, "the Natron Utility tab has a UV Prim parameter")
            prim.set(uv_prim)
            tile = utility_knob("UdimTile")
            require(tile is not None, "the Natron Utility tab has a UDIM Tile parameter")
            tile.set(1001)
            primvar = utility_knob("UvPrimvar")
            require(primvar is not None, "the Natron Utility tab has a UV Primvar parameter")
            primvar.set("st")

    def main():
        yield ("sleep", SETTLE_MS)
        section("setup")
        require(settings.connectInput(0, read), "connect the USDRenderSettings to the USDRead")
        require(render.connectInput(0, settings), "connect the USDRender to the USDRenderSettings")
        read.getParam("filename").setValue(ring_path)
        ok = yield ("until", lambda: settings.getParam("camera") is not None and "/World/Cam" in options(settings.getParam("camera")),
                    POLL_TIMEOUT_S)
        check(ok, "the settings' Camera menu lists /World/Cam")
        if ok:
            choose(settings.getParam("camera"), "/World/Cam")

        section("spherical")
        set_format("m288latlong", LATLONG)
        setup_case(ring_path, "spherical")
        yield ("sleep", SETTLE_MS)
        check_image("m288-spherical", render_to_png("m288-spherical"))
        yield from show_and_shoot("m288-spherical")

        section("cylindrical")
        setup_case(ring_path, "cylindrical")
        yield ("sleep", SETTLE_MS)
        check_image("m288-cylindrical", render_to_png("m288-cylindrical"))
        yield from show_and_shoot("m288-cylindrical")

        section("overscan")
        set_format("m288persp", PERSPECTIVE)
        setup_case(ring_path, "camera", OVERSCAN_PX)
        yield ("sleep", SETTLE_MS)
        result = render_to_png("m288-overscan")
        check_image("m288-overscan", result)
        if result is not None:
            expected = (PERSPECTIVE[0] + 2 * OVERSCAN_PX, PERSPECTIVE[1] + 2 * OVERSCAN_PX)
            check((result[0], result[1]) == expected,
                  "the overscanned render is %dx%d, the format grown by %d px on every side (got %dx%d)"
                  % (expected + (OVERSCAN_PX, result[0], result[1])))
        yield from show_and_shoot("m288-overscan")

        section("uv unwrap")
        set_format("m288uv", UV_TILE)
        setup_case(uv_path, "uvUnwrap", 0, "/World/Cube")
        yield ("sleep", SETTLE_MS)
        result = render_to_png("m288-uv-unwrap")
        check_image("m288-uv-unwrap", result)
        if result is not None:
            check(result[2] < 0.9, "the bake leaves texels outside the cube's UV layout empty (%.1f%% coloured)" % (result[2] * 100.0))
        yield from show_and_shoot("m288-uv-unwrap")

    run(main())
except Exception:
    from guitest import check as _check, finish as _finish
    _check(False, "script error: " + traceback.format_exc())
    _finish()
